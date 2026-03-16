//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/udp_service.hpp>

#include <boost/corosio/native/detail/epoll/epoll_udp_socket.hpp>
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>
#include <mutex>

#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** epoll UDP service implementation.

    Inherits from udp_service to enable runtime polymorphism.
    Uses key_type = udp_service for service lookup.
*/
class BOOST_COROSIO_DECL epoll_udp_service final
    : public reactor_socket_service<
          epoll_udp_service,
          udp_service,
          epoll_scheduler,
          epoll_udp_socket>
{
public:
    explicit epoll_udp_service(capy::execution_context& ctx)
        : reactor_socket_service(ctx)
    {
    }

    std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;
    std::error_code
    bind_datagram(udp_socket::implementation& impl, endpoint ep) override;
};

// Cancellation for connectionless ops

inline void
epoll_send_to_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_recv_from_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

// Cancellation for connected-mode ops

inline void
epoll_udp_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_send_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_recv_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

// Completion handlers

inline void
epoll_datagram_op::operator()()
{
    complete_io_op(*this);
}

inline void
epoll_recv_from_op::operator()()
{
    complete_datagram_op(*this, this->source_out);
}

inline void
epoll_udp_connect_op::operator()()
{
    complete_connect_op(*this);
}

inline void
epoll_recv_op::operator()()
{
    complete_io_op(*this);
}

// Socket construction/destruction

inline epoll_udp_socket::epoll_udp_socket(epoll_udp_service& svc) noexcept
    : reactor_datagram_socket(svc)
{
}

inline epoll_udp_socket::~epoll_udp_socket() = default;

// Connectionless I/O

inline std::coroutine_handle<>
epoll_udp_socket::send_to(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    endpoint dest,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_send_to(h, ex, buf, dest, token, ec, bytes_out);
}

inline std::coroutine_handle<>
epoll_udp_socket::recv_from(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    endpoint* source,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_recv_from(h, ex, buf, source, token, ec, bytes_out);
}

// Connected-mode I/O

inline std::coroutine_handle<>
epoll_udp_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return do_connect(h, ex, ep, token, ec);
}

inline std::coroutine_handle<>
epoll_udp_socket::send(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_send(h, ex, buf, token, ec, bytes_out);
}

inline std::coroutine_handle<>
epoll_udp_socket::recv(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_recv(h, ex, buf, token, ec, bytes_out);
}

inline endpoint
epoll_udp_socket::remote_endpoint() const noexcept
{
    return reactor_datagram_socket::remote_endpoint();
}

inline void
epoll_udp_socket::cancel() noexcept
{
    do_cancel();
}

inline void
epoll_udp_socket::close_socket() noexcept
{
    do_close_socket();
}

inline std::error_code
epoll_udp_service::open_datagram_socket(
    udp_socket::implementation& impl, int family, int type, int protocol)
{
    auto* epoll_impl = static_cast<epoll_udp_socket*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return make_err(errno);

    if (family == AF_INET6)
    {
        int one = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }

    epoll_impl->fd_ = fd;

    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op    = nullptr;
        epoll_impl->desc_state_.write_op   = nullptr;
        epoll_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

inline std::error_code
epoll_udp_service::bind_datagram(udp_socket::implementation& impl, endpoint ep)
{
    return static_cast<epoll_udp_socket*>(&impl)->do_bind(ep);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_UDP_SERVICE_HPP
