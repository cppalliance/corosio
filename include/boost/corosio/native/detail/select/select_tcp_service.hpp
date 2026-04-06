//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/tcp_service.hpp>

#include <boost/corosio/native/detail/select/select_tcp_socket.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>
#include <mutex>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    Each I/O op tries the syscall speculatively; only registers with
    the reactor on EAGAIN. Fd is registered once at open time and
    stays registered until close. The reactor only marks ready_events_;
    actual I/O happens in invoke_deferred_io(). cancel() captures
    shared_from_this() into op.impl_ptr to keep the impl alive.
*/

namespace boost::corosio::detail {

/** select TCP service implementation.

    Inherits from tcp_service to enable runtime polymorphism.
    Uses key_type = tcp_service for service lookup.
*/
class BOOST_COROSIO_DECL select_tcp_service final
    : public reactor_socket_service<
          select_tcp_service,
          tcp_service,
          select_scheduler,
          select_tcp_socket>
{
public:
    explicit select_tcp_service(capy::execution_context& ctx)
        : reactor_socket_service(ctx)
    {
    }

    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;

    std::error_code
    bind_socket(tcp_socket::implementation& impl, endpoint ep) override;
};

inline void
select_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_op::operator()()
{
    complete_io_op(*this);
}

inline void
select_connect_op::operator()()
{
    complete_connect_op(*this);
}

inline select_tcp_socket::select_tcp_socket(select_tcp_service& svc) noexcept
    : reactor_stream_socket(svc)
{
}

inline select_tcp_socket::~select_tcp_socket() = default;

inline std::coroutine_handle<>
select_tcp_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    auto result = do_connect(h, ex, ep, token, ec);
    // Rebuild fd_sets so select() watches for writability
    if (result == std::noop_coroutine())
        svc_.scheduler().notify_reactor();
    return result;
}

inline std::coroutine_handle<>
select_tcp_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_read_some(h, ex, param, token, ec, bytes_out);
}

inline std::coroutine_handle<>
select_tcp_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto result = do_write_some(h, ex, param, token, ec, bytes_out);
    // Rebuild fd_sets so select() watches for writability
    if (result == std::noop_coroutine())
        svc_.scheduler().notify_reactor();
    return result;
}

inline void
select_tcp_socket::cancel() noexcept
{
    do_cancel();
}

inline void
select_tcp_socket::close_socket() noexcept
{
    do_close_socket();
}

inline std::error_code
select_tcp_service::open_socket(
    tcp_socket::implementation& impl, int family, int type, int protocol)
{
    auto* select_impl = static_cast<select_tcp_socket*>(&impl);
    select_impl->close_socket();

    int fd = ::socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int one = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }

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

#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif

    select_impl->fd_ = fd;

    select_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(select_impl->desc_state_.mutex);
        select_impl->desc_state_.read_op    = nullptr;
        select_impl->desc_state_.write_op   = nullptr;
        select_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &select_impl->desc_state_);

    return {};
}

inline std::error_code
select_tcp_service::bind_socket(
    tcp_socket::implementation& impl, endpoint ep)
{
    return static_cast<select_tcp_socket*>(&impl)->do_bind(ep);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_TCP_SERVICE_HPP
