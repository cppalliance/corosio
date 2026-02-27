//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/native/native_socket_option.hpp>

#include <cstring>

namespace boost::corosio::socket_option {

// no_delay

int
no_delay::level() noexcept
{
    return native_socket_option::no_delay::level();
}
int
no_delay::name() noexcept
{
    return native_socket_option::no_delay::name();
}

// keep_alive

int
keep_alive::level() noexcept
{
    return native_socket_option::keep_alive::level();
}
int
keep_alive::name() noexcept
{
    return native_socket_option::keep_alive::name();
}

// v6_only

int
v6_only::level() noexcept
{
    return native_socket_option::v6_only::level();
}
int
v6_only::name() noexcept
{
    return native_socket_option::v6_only::name();
}

// reuse_address

int
reuse_address::level() noexcept
{
    return native_socket_option::reuse_address::level();
}
int
reuse_address::name() noexcept
{
    return native_socket_option::reuse_address::name();
}

// reuse_port

#ifdef SO_REUSEPORT
int
reuse_port::level() noexcept
{
    return native_socket_option::reuse_port::level();
}
int
reuse_port::name() noexcept
{
    return native_socket_option::reuse_port::name();
}
#else
int
reuse_port::level() noexcept
{
    return SOL_SOCKET;
}
int
reuse_port::name() noexcept
{
    return -1;
}
#endif

// receive_buffer_size

int
receive_buffer_size::level() noexcept
{
    return native_socket_option::receive_buffer_size::level();
}
int
receive_buffer_size::name() noexcept
{
    return native_socket_option::receive_buffer_size::name();
}

// send_buffer_size

int
send_buffer_size::level() noexcept
{
    return native_socket_option::send_buffer_size::level();
}
int
send_buffer_size::name() noexcept
{
    return native_socket_option::send_buffer_size::name();
}

// linger

linger::linger(bool enabled, int timeout) noexcept
{
    native_socket_option::linger native(enabled, timeout);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform linger exceeds socket_option::linger storage");
    std::memcpy(storage_, native.data(), native.size());
}

bool
linger::enabled() const noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(), storage_, native.size());
    return native.enabled();
}

void
linger::enabled(bool e) noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(), storage_, native.size());
    native.enabled(e);
    std::memcpy(storage_, native.data(), native.size());
}

int
linger::timeout() const noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(), storage_, native.size());
    return native.timeout();
}

void
linger::timeout(int t) noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(), storage_, native.size());
    native.timeout(t);
    std::memcpy(storage_, native.data(), native.size());
}

int
linger::level() noexcept
{
    return native_socket_option::linger::level();
}
int
linger::name() noexcept
{
    return native_socket_option::linger::name();
}

std::size_t
linger::size() const noexcept
{
    return native_socket_option::linger{}.size();
}

} // namespace boost::corosio::socket_option
