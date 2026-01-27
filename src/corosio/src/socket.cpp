//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket.hpp>
#include <boost/corosio/detail/except.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)
#include "src/detail/iocp/sockets.hpp"
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/sockets.hpp"
#endif

namespace boost::corosio {

namespace {
#if defined(BOOST_COROSIO_BACKEND_IOCP)
using socket_service = detail::win_sockets;
using socket_impl_type = detail::win_socket_impl;
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
using socket_service = detail::epoll_sockets;
using socket_impl_type = detail::epoll_socket_impl;
#endif
} // namespace

socket::
~socket()
{
    close();
}

socket::
socket(
    capy::execution_context& ctx)
    : io_stream(ctx)
{
}

void
socket::
open()
{
    if (impl_)
        return;

    auto& svc = ctx_->use_service<socket_service>();
    auto& wrapper = svc.create_impl();
    impl_ = &wrapper;

#if defined(BOOST_COROSIO_BACKEND_IOCP)
    system::error_code ec = svc.open_socket(*wrapper.get_internal());
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    system::error_code ec = svc.open_socket(wrapper);
#endif
    if (ec)
    {
        wrapper.release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "socket::open");
    }
}

void
socket::
close()
{
    if (!impl_)
        return;

    auto* wrapper = static_cast<socket_impl_type*>(impl_);
    wrapper->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    if (!impl_)
        return;
#if defined(BOOST_COROSIO_BACKEND_IOCP)
    static_cast<socket_impl_type*>(impl_)->get_internal()->cancel();
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
    static_cast<socket_impl_type*>(impl_)->cancel();
#endif
}

void
socket::
shutdown(shutdown_type what)
{
    if (impl_)
        get().shutdown(what);
}

native_handle_type
socket::
native_handle() const noexcept
{
    if (!impl_)
    {
#if defined(BOOST_COROSIO_BACKEND_IOCP)
        return static_cast<native_handle_type>(~0ull);  // INVALID_SOCKET
#else
        return -1;
#endif
    }
    return get().native_handle();
}

//------------------------------------------------------------------------------
// Socket Options
//------------------------------------------------------------------------------

void
socket::
set_no_delay(bool value)
{
    if (!impl_)
        detail::throw_logic_error("set_no_delay: socket not open");
    system::error_code ec = get().set_no_delay(value);
    if (ec)
        detail::throw_system_error(ec, "socket::set_no_delay");
}

bool
socket::
no_delay() const
{
    if (!impl_)
        detail::throw_logic_error("no_delay: socket not open");
    system::error_code ec;
    bool result = get().no_delay(ec);
    if (ec)
        detail::throw_system_error(ec, "socket::no_delay");
    return result;
}

void
socket::
set_keep_alive(bool value)
{
    if (!impl_)
        detail::throw_logic_error("set_keep_alive: socket not open");
    system::error_code ec = get().set_keep_alive(value);
    if (ec)
        detail::throw_system_error(ec, "socket::set_keep_alive");
}

bool
socket::
keep_alive() const
{
    if (!impl_)
        detail::throw_logic_error("keep_alive: socket not open");
    system::error_code ec;
    bool result = get().keep_alive(ec);
    if (ec)
        detail::throw_system_error(ec, "socket::keep_alive");
    return result;
}

void
socket::
set_receive_buffer_size(int size)
{
    if (!impl_)
        detail::throw_logic_error("set_receive_buffer_size: socket not open");
    system::error_code ec = get().set_receive_buffer_size(size);
    if (ec)
        detail::throw_system_error(ec, "socket::set_receive_buffer_size");
}

int
socket::
receive_buffer_size() const
{
    if (!impl_)
        detail::throw_logic_error("receive_buffer_size: socket not open");
    system::error_code ec;
    int result = get().receive_buffer_size(ec);
    if (ec)
        detail::throw_system_error(ec, "socket::receive_buffer_size");
    return result;
}

void
socket::
set_send_buffer_size(int size)
{
    if (!impl_)
        detail::throw_logic_error("set_send_buffer_size: socket not open");
    system::error_code ec = get().set_send_buffer_size(size);
    if (ec)
        detail::throw_system_error(ec, "socket::set_send_buffer_size");
}

int
socket::
send_buffer_size() const
{
    if (!impl_)
        detail::throw_logic_error("send_buffer_size: socket not open");
    system::error_code ec;
    int result = get().send_buffer_size(ec);
    if (ec)
        detail::throw_system_error(ec, "socket::send_buffer_size");
    return result;
}

void
socket::
set_linger(bool enabled, int timeout)
{
    if (!impl_)
        detail::throw_logic_error("set_linger: socket not open");
    system::error_code ec = get().set_linger(enabled, timeout);
    if (ec)
        detail::throw_system_error(ec, "socket::set_linger");
}

socket::linger_options
socket::
linger() const
{
    if (!impl_)
        detail::throw_logic_error("linger: socket not open");
    system::error_code ec;
    linger_options result = get().linger(ec);
    if (ec)
        detail::throw_system_error(ec, "socket::linger");
    return result;
}

endpoint
socket::
local_endpoint() const noexcept
{
    if (!impl_)
        return endpoint{};
    return get().local_endpoint();
}

endpoint
socket::
remote_endpoint() const noexcept
{
    if (!impl_)
        return endpoint{};
    return get().remote_endpoint();
}

} // namespace boost::corosio
