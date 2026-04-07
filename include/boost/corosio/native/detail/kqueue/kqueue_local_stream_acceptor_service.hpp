//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_local_stream_acceptor.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_local_stream_service.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <cassert>
#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace boost::corosio::detail {

/* kqueue local stream acceptor service implementation.

   Inherits from local_stream_acceptor_service to enable runtime
   polymorphism. Uses key_type = local_stream_acceptor_service
   for service lookup.
*/
class BOOST_COROSIO_DECL kqueue_local_stream_acceptor_service final
    : public reactor_acceptor_service<
          kqueue_local_stream_acceptor_service,
          local_stream_acceptor_service,
          kqueue_scheduler,
          kqueue_local_stream_acceptor,
          kqueue_local_stream_service>
{
    using base_type = reactor_acceptor_service<
        kqueue_local_stream_acceptor_service,
        local_stream_acceptor_service,
        kqueue_scheduler,
        kqueue_local_stream_acceptor,
        kqueue_local_stream_service>;
    friend base_type;

public:
    explicit kqueue_local_stream_acceptor_service(capy::execution_context& ctx);
    ~kqueue_local_stream_acceptor_service() override;

    std::error_code open_acceptor_socket(
        local_stream_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override;
    std::error_code bind_acceptor(
        local_stream_acceptor::implementation& impl,
        corosio::local_endpoint ep) override;
    std::error_code listen_acceptor(
        local_stream_acceptor::implementation& impl,
        int backlog) override;
};

inline void
kqueue_local_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_accept_op::operator()()
{
    complete_accept_op<kqueue_local_stream_socket>(*this);
}

inline kqueue_local_stream_acceptor::kqueue_local_stream_acceptor(
    kqueue_local_stream_acceptor_service& svc) noexcept
    : reactor_acceptor(svc)
{
}

inline std::coroutine_handle<>
kqueue_local_stream_acceptor::accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h        = h;
    op.ex       = ex;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.fd       = fd_;
    op.start(token, this);

    sockaddr_storage peer_storage{};
    socklen_t addrlen;

    int accepted;
    do
    {
        addrlen = sizeof(peer_storage);
        accepted = ::accept(
            fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen);
    }
    while (accepted < 0 && errno == EINTR);

    if (accepted >= 0)
    {
        // Set non-blocking and close-on-exec on the accepted socket
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags == -1 || ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int errn = errno;
            ::close(accepted);
            op.complete(errn, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }
        if (::fcntl(accepted, F_SETFD, FD_CLOEXEC) == -1)
        {
            int errn = errno;
            ::close(accepted);
            op.complete(errn, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        // SO_NOSIGPIPE before budget check so both inline and
        // queued paths have it applied (macOS lacks MSG_NOSIGNAL)
#ifdef SO_NOSIGPIPE
        int one = 1;
        if (::setsockopt(
                accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == -1)
        {
            int errn = errno;
            ::close(accepted);
            op.complete(errn, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }
#endif

        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }

        if (svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = svc_.stream_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<kqueue_local_stream_socket&>(
                        *socket_svc->construct());
                impl.set_socket(accepted);

                impl.desc_state_.fd = accepted;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op    = nullptr;
                    impl.desc_state_.write_op   = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(
                    accepted, &impl.desc_state_);

                impl.set_endpoints(
                    local_endpoint_,
                    from_sockaddr_as(peer_storage, addrlen, corosio::local_endpoint{}));

                *ec = {};
                if (impl_out)
                    *impl_out = &impl;
            }
            else
            {
                ::close(accepted);
                *ec = make_err(ENOENT);
                if (impl_out)
                    *impl_out = nullptr;
            }
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }

        op.accepted_fd   = accepted;
        op.peer_storage  = peer_storage;
        op.peer_addrlen  = addrlen;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        op.impl_ptr = shared_from_this();
        svc_.work_started();

        std::lock_guard lock(desc_state_.mutex);
        bool io_done = false;
        if (desc_state_.read_ready)
        {
            desc_state_.read_ready = false;
            op.perform_io();
            io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
            if (!io_done)
                op.errn = 0;
        }

        if (io_done || op.cancelled.load(std::memory_order_acquire))
        {
            svc_.post(&op);
            svc_.work_finished();
        }
        else
        {
            desc_state_.read_op = &op;
        }
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
    return std::noop_coroutine();
}

inline void
kqueue_local_stream_acceptor::cancel() noexcept
{
    do_cancel();
}

inline void
kqueue_local_stream_acceptor::close_socket() noexcept
{
    do_close_socket();
}

inline native_handle_type
kqueue_local_stream_acceptor::release_socket() noexcept
{
    return this->do_release_socket();
}

inline kqueue_local_stream_acceptor_service::
    kqueue_local_stream_acceptor_service(capy::execution_context& ctx)
    : base_type(ctx)
{
    auto* svc = ctx_.find_service<detail::local_stream_service>();
    stream_svc_ = svc
        ? dynamic_cast<kqueue_local_stream_service*>(svc)
        : nullptr;
    assert(stream_svc_ &&
        "local_stream_service must be registered before acceptor service");
}

inline kqueue_local_stream_acceptor_service::
    ~kqueue_local_stream_acceptor_service() {}

inline std::error_code
kqueue_local_stream_acceptor_service::open_acceptor_socket(
    local_stream_acceptor::implementation& impl,
    int family,
    int type,
    int protocol)
{
    auto* kq_impl = static_cast<kqueue_local_stream_acceptor*>(&impl);
    kq_impl->close_socket();

    int fd = ::socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking and close-on-exec
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // SO_NOSIGPIPE on macOS (where MSG_NOSIGNAL doesn't exist)
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

    kq_impl->fd_ = fd;

    // Set up descriptor state but do NOT register with kqueue yet
    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op = nullptr;
    }

    return {};
}

inline std::error_code
kqueue_local_stream_acceptor_service::bind_acceptor(
    local_stream_acceptor::implementation& impl, corosio::local_endpoint ep)
{
    return static_cast<kqueue_local_stream_acceptor*>(&impl)->do_bind(ep);
}

inline std::error_code
kqueue_local_stream_acceptor_service::listen_acceptor(
    local_stream_acceptor::implementation& impl, int backlog)
{
    return static_cast<kqueue_local_stream_acceptor*>(&impl)->do_listen(backlog);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
