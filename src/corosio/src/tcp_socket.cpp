//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include "src/detail/iocp/sockets.hpp"
#else
#include "src/detail/socket_service.hpp"
#endif

namespace boost::corosio {

tcp_socket::
~tcp_socket()
{
    close();
}

tcp_socket::
tcp_socket(
    capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_stream(create_handle<detail::win_sockets>(ctx))
#else
    : io_stream(create_handle<detail::socket_service>(ctx))
#endif
{
}

void
tcp_socket::
open()
{
    if (is_open())
        return;
#if BOOST_COROSIO_HAS_IOCP
    auto& svc = static_cast<detail::win_sockets&>(h_.service());
    auto& wrapper = static_cast<tcp_socket::socket_impl&>(*h_.get());
    std::error_code ec = svc.open_socket(
        *static_cast<detail::win_socket_impl&>(wrapper).get_internal());
#else
    auto& svc = static_cast<detail::socket_service&>(h_.service());
    std::error_code ec = svc.open_socket(
        static_cast<tcp_socket::socket_impl&>(*h_.get()));
#endif
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::open");
}

void
tcp_socket::
close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
tcp_socket::
cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

void
tcp_socket::
shutdown(shutdown_type what)
{
    if (is_open())
        get().shutdown(what);
}

native_handle_type
tcp_socket::
native_handle() const noexcept
{
    if (!is_open())
    {
#if BOOST_COROSIO_HAS_IOCP
        return static_cast<native_handle_type>(~0ull);  // INVALID_SOCKET
#else
        return -1;
#endif
    }
    return get().native_handle();
}

void
tcp_socket::
set_no_delay(bool value)
{
    if (!is_open())
        detail::throw_logic_error("set_no_delay: socket not open");
    std::error_code ec = get().set_no_delay(value);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::set_no_delay");
}

bool
tcp_socket::
no_delay() const
{
    if (!is_open())
        detail::throw_logic_error("no_delay: socket not open");
    std::error_code ec;
    bool result = get().no_delay(ec);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::no_delay");
    return result;
}

void
tcp_socket::
set_keep_alive(bool value)
{
    if (!is_open())
        detail::throw_logic_error("set_keep_alive: socket not open");
    std::error_code ec = get().set_keep_alive(value);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::set_keep_alive");
}

bool
tcp_socket::
keep_alive() const
{
    if (!is_open())
        detail::throw_logic_error("keep_alive: socket not open");
    std::error_code ec;
    bool result = get().keep_alive(ec);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::keep_alive");
    return result;
}

void
tcp_socket::
set_receive_buffer_size(int size)
{
    if (!is_open())
        detail::throw_logic_error("set_receive_buffer_size: socket not open");
    std::error_code ec = get().set_receive_buffer_size(size);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::set_receive_buffer_size");
}

int
tcp_socket::
receive_buffer_size() const
{
    if (!is_open())
        detail::throw_logic_error("receive_buffer_size: socket not open");
    std::error_code ec;
    int result = get().receive_buffer_size(ec);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::receive_buffer_size");
    return result;
}

void
tcp_socket::
set_send_buffer_size(int size)
{
    if (!is_open())
        detail::throw_logic_error("set_send_buffer_size: socket not open");
    std::error_code ec = get().set_send_buffer_size(size);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::set_send_buffer_size");
}

int
tcp_socket::
send_buffer_size() const
{
    if (!is_open())
        detail::throw_logic_error("send_buffer_size: socket not open");
    std::error_code ec;
    int result = get().send_buffer_size(ec);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::send_buffer_size");
    return result;
}

void
tcp_socket::
set_linger(bool enabled, int timeout)
{
    if (!is_open())
        detail::throw_logic_error("set_linger: socket not open");
    std::error_code ec = get().set_linger(enabled, timeout);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::set_linger");
}

tcp_socket::linger_options
tcp_socket::
linger() const
{
    if (!is_open())
        detail::throw_logic_error("linger: socket not open");
    std::error_code ec;
    linger_options result = get().linger(ec);
    if (ec)
        detail::throw_system_error(ec, "tcp_socket::linger");
    return result;
}

endpoint
tcp_socket::
local_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().local_endpoint();
}

endpoint
tcp_socket::
remote_endpoint() const noexcept
{
    if (!is_open())
        return endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio
