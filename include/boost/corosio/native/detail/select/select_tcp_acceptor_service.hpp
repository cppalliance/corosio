//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/tcp_acceptor_service.hpp>

#include <boost/corosio/native/detail/select/select_tcp_acceptor.hpp>
#include <boost/corosio/native/detail/select/select_tcp_service.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/// State for select acceptor service.
using select_tcp_acceptor_state =
    reactor_service_state<select_scheduler, select_tcp_acceptor>;

/** select acceptor service implementation.

    Inherits from tcp_acceptor_service to enable runtime polymorphism.
    Uses key_type = tcp_acceptor_service for service lookup.
*/
class BOOST_COROSIO_DECL select_tcp_acceptor_service final
    : public tcp_acceptor_service
{
public:
    explicit select_tcp_acceptor_service(capy::execution_context& ctx);
    ~select_tcp_acceptor_service() override;

    select_tcp_acceptor_service(select_tcp_acceptor_service const&) = delete;
    select_tcp_acceptor_service&
    operator=(select_tcp_acceptor_service const&) = delete;

    void shutdown() override;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;
    void close(io_object::handle&) override;
    std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override;
    std::error_code
    bind_acceptor(tcp_acceptor::implementation& impl, endpoint ep) override;
    std::error_code
    listen_acceptor(tcp_acceptor::implementation& impl, int backlog) override;

    select_scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }
    void post(scheduler_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the TCP service for creating peer sockets during accept. */
    select_tcp_service* tcp_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<select_tcp_acceptor_state> state_;
};

inline void
select_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_accept_op::operator()()
{
    complete_accept_op<select_tcp_socket>(*this);
}

inline select_tcp_acceptor::select_tcp_acceptor(
    select_tcp_acceptor_service& svc) noexcept
    : reactor_acceptor(svc)
{
}

inline std::coroutine_handle<>
select_tcp_acceptor::accept(
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
    socklen_t addrlen = sizeof(peer_storage);
    int accepted;
    do
    {
        accepted =
            ::accept(fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen);
    }
    while (accepted < 0 && errno == EINTR);

    if (accepted >= 0)
    {
        if (accepted >= FD_SETSIZE)
        {
            ::close(accepted);
            op.complete(EINVAL, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags == -1)
        {
            int err = errno;
            ::close(accepted);
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        if (::fcntl(accepted, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        if (::fcntl(accepted, F_SETFD, FD_CLOEXEC) == -1)
        {
            int err = errno;
            ::close(accepted);
            op.complete(err, 0);
            op.impl_ptr = shared_from_this();
            svc_.post(&op);
            return std::noop_coroutine();
        }

        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.read_ready = false;
        }

        if (svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = svc_.tcp_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<select_tcp_socket&>(*socket_svc->construct());
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
                    local_endpoint_, from_sockaddr(peer_storage));

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

        op.accepted_fd  = accepted;
        op.peer_storage = peer_storage;
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
select_tcp_acceptor::cancel() noexcept
{
    do_cancel();
}

inline void
select_tcp_acceptor::close_socket() noexcept
{
    do_close_socket();
}

inline select_tcp_acceptor_service::select_tcp_acceptor_service(
    capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(
          std::make_unique<select_tcp_acceptor_state>(
              ctx.use_service<select_scheduler>()))
{
}

inline select_tcp_acceptor_service::~select_tcp_acceptor_service() {}

inline void
select_tcp_acceptor_service::shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->impl_list_.pop_front())
        impl->close_socket();

    // Don't clear impl_ptrs_ here — same rationale as
    // select_tcp_service::shutdown(). Let ~state_ release ptrs
    // after scheduler shutdown has drained all queued ops.
}

inline io_object::implementation*
select_tcp_acceptor_service::construct()
{
    auto impl = std::make_shared<select_tcp_acceptor>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->impl_ptrs_.emplace(raw, std::move(impl));
    state_->impl_list_.push_back(raw);

    return raw;
}

inline void
select_tcp_acceptor_service::destroy(io_object::implementation* impl)
{
    auto* select_impl = static_cast<select_tcp_acceptor*>(impl);
    select_impl->close_socket();
    std::lock_guard lock(state_->mutex_);
    state_->impl_list_.remove(select_impl);
    state_->impl_ptrs_.erase(select_impl);
}

inline void
select_tcp_acceptor_service::close(io_object::handle& h)
{
    static_cast<select_tcp_acceptor*>(h.get())->close_socket();
}

inline std::error_code
select_tcp_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl, int family, int type, int protocol)
{
    auto* select_impl = static_cast<select_tcp_acceptor*>(&impl);
    select_impl->close_socket();

    int fd = ::socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

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

    if (fd >= FD_SETSIZE)
    {
        ::close(fd);
        return make_err(EMFILE);
    }

    if (family == AF_INET6)
    {
        int val = 0; // dual-stack default
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
    }

#ifdef SO_NOSIGPIPE
    {
        int nosig = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
    }
#endif

    select_impl->fd_ = fd;

    // Set up descriptor state but do NOT register with reactor yet
    // (registration happens in do_listen via reactor_acceptor base)
    select_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(select_impl->desc_state_.mutex);
        select_impl->desc_state_.read_op = nullptr;
    }

    return {};
}

inline std::error_code
select_tcp_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    return static_cast<select_tcp_acceptor*>(&impl)->do_bind(ep);
}

inline std::error_code
select_tcp_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    return static_cast<select_tcp_acceptor*>(&impl)->do_listen(backlog);
}

inline void
select_tcp_acceptor_service::post(scheduler_op* op)
{
    state_->sched_.post(op);
}

inline void
select_tcp_acceptor_service::work_started() noexcept
{
    state_->sched_.work_started();
}

inline void
select_tcp_acceptor_service::work_finished() noexcept
{
    state_->sched_.work_finished();
}

inline select_tcp_service*
select_tcp_acceptor_service::tcp_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::tcp_service>();
    return svc ? dynamic_cast<select_tcp_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_ACCEPTOR_SERVICE_HPP
