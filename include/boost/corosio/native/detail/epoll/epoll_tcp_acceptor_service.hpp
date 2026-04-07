//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/tcp_acceptor_service.hpp>

#include <boost/corosio/native/detail/epoll/epoll_tcp_acceptor.hpp>
#include <boost/corosio/native/detail/epoll/epoll_tcp_service.hpp>
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <memory>
#include <mutex>
#include <utility>

#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** epoll acceptor service implementation.

    Derives from reactor_acceptor_service for shared construct/
    destroy/shutdown/close logic. Provides epoll-specific socket
    creation (SOCK_NONBLOCK | SOCK_CLOEXEC) and dual-stack defaults.
    Uses key_type = tcp_acceptor_service for service lookup.
*/
class BOOST_COROSIO_DECL epoll_tcp_acceptor_service final
    : public reactor_acceptor_service<
          epoll_tcp_acceptor_service,
          tcp_acceptor_service,
          epoll_scheduler,
          epoll_tcp_acceptor,
          epoll_tcp_service>
{
    using base_type = reactor_acceptor_service<
        epoll_tcp_acceptor_service,
        tcp_acceptor_service,
        epoll_scheduler,
        epoll_tcp_acceptor,
        epoll_tcp_service>;
    friend base_type;

public:
    explicit epoll_tcp_acceptor_service(capy::execution_context& ctx);
    ~epoll_tcp_acceptor_service() override;

    std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override;
    std::error_code
    bind_acceptor(tcp_acceptor::implementation& impl, endpoint ep) override;
    std::error_code
    listen_acceptor(tcp_acceptor::implementation& impl, int backlog) override;
};

inline void
epoll_accept_op::cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_accept_op::operator()()
{
    complete_accept_op<epoll_tcp_socket>(*this);
}

inline epoll_tcp_acceptor::epoll_tcp_acceptor(
    epoll_tcp_acceptor_service& svc) noexcept
    : reactor_acceptor(svc)
{
}

inline std::coroutine_handle<>
epoll_tcp_acceptor::accept(
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
        accepted = ::accept4(
            fd_, reinterpret_cast<sockaddr*>(&peer_storage), &addrlen,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
    }
    while (accepted < 0 && errno == EINTR);

    if (accepted >= 0)
    {
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
                    static_cast<epoll_tcp_socket&>(*socket_svc->construct());
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
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline void
epoll_tcp_acceptor::cancel() noexcept
{
    do_cancel();
}

inline void
epoll_tcp_acceptor::close_socket() noexcept
{
    do_close_socket();
}

inline epoll_tcp_acceptor_service::epoll_tcp_acceptor_service(
    capy::execution_context& ctx)
    : base_type(ctx)
{
    auto* svc = ctx_.find_service<detail::tcp_service>();
    stream_svc_ = svc ? dynamic_cast<epoll_tcp_service*>(svc) : nullptr;
}

inline epoll_tcp_acceptor_service::~epoll_tcp_acceptor_service() {}

inline std::error_code
epoll_tcp_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl, int family, int type, int protocol)
{
    auto* epoll_impl = static_cast<epoll_tcp_acceptor*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int val = 0; // dual-stack default
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
    }

    epoll_impl->fd_ = fd;

    // Set up descriptor state but do NOT register with epoll yet
    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op = nullptr;
    }

    return {};
}

inline std::error_code
epoll_tcp_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    return static_cast<epoll_tcp_acceptor*>(&impl)->do_bind(ep);
}

inline std::error_code
epoll_tcp_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    return static_cast<epoll_tcp_acceptor*>(&impl)->do_listen(backlog);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_TCP_ACCEPTOR_SERVICE_HPP
