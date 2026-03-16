//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/udp_service.hpp>

#include <boost/corosio/native/detail/select/select_udp_socket.hpp>
#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>
#include <mutex>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** select UDP service implementation.

    Inherits from udp_service to enable runtime polymorphism.
    Uses key_type = udp_service for service lookup.
*/
class BOOST_COROSIO_DECL select_udp_service final
    : public reactor_socket_service<
          select_udp_service,
          udp_service,
          select_scheduler,
          select_udp_socket>
{
public:
    explicit select_udp_service(capy::execution_context& ctx)
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

inline void
select_send_to_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_recv_from_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
select_datagram_op::operator()()
{
    complete_io_op(*this);
}

inline void
select_recv_from_op::operator()()
{
    complete_datagram_op(*this, this->source_out);
}

inline select_udp_socket::select_udp_socket(select_udp_service& svc) noexcept
    : reactor_datagram_socket(svc)
{
}

inline select_udp_socket::~select_udp_socket() = default;

inline std::coroutine_handle<>
select_udp_socket::send_to(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    endpoint dest,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto result = do_send_to(h, ex, buf, dest, token, ec, bytes_out);
    if (result == std::noop_coroutine())
        svc_.scheduler().notify_reactor();
    return result;
}

inline std::coroutine_handle<>
select_udp_socket::recv_from(
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

inline void
select_udp_socket::cancel() noexcept
{
    do_cancel();
}

inline void
select_udp_socket::close_socket() noexcept
{
    do_close_socket();
}

inline std::error_code
select_udp_service::open_datagram_socket(
    udp_socket::implementation& impl, int family, int type, int protocol)
{
    auto* select_impl = static_cast<select_udp_socket*>(&impl);
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
select_udp_service::bind_datagram(udp_socket::implementation& impl, endpoint ep)
{
    return static_cast<select_udp_socket*>(&impl)->do_bind(ep);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_UDP_SERVICE_HPP
