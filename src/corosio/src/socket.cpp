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


#if defined(_WIN32)
#include "src/detail/iocp/sockets.hpp"
#else
// POSIX backends use the abstract socket_service interface
#include "src/detail/socket_service.hpp"
#endif

namespace boost::corosio {

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

#if defined(_WIN32)
    auto& svc = ctx_->use_service<detail::win_sockets>();
    auto& wrapper = svc.create_impl();
    impl_ = &wrapper;
    system::error_code ec = svc.open_socket(*wrapper.get_internal());
#else
    // POSIX backends use abstract socket_service for runtime polymorphism.
    // The concrete service (epoll_sockets or select_sockets) must be installed
    // by the context constructor before any socket operations.
    auto* svc = ctx_->find_service<detail::socket_service>();
    if (!svc)
        detail::throw_logic_error("socket::open: no socket service installed");
    auto& wrapper = svc->create_impl();
    impl_ = &wrapper;
    system::error_code ec = svc->open_socket(wrapper);
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

    // socket_impl has virtual release() method
    impl_->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    if (!impl_)
        return;
#if defined(_WIN32)
    static_cast<detail::win_socket_impl*>(impl_)->get_internal()->cancel();
#else
    // socket_impl has virtual cancel() method
    get().cancel();
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
#if defined(_WIN32)
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
