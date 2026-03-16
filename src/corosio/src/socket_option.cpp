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

// broadcast

int
broadcast::level() noexcept
{
    return native_socket_option::broadcast::level();
}
int
broadcast::name() noexcept
{
    return native_socket_option::broadcast::name();
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

// multicast_loop_v4

int
multicast_loop_v4::level() noexcept
{
    return native_socket_option::multicast_loop_v4::level();
}
int
multicast_loop_v4::name() noexcept
{
    return native_socket_option::multicast_loop_v4::name();
}

// multicast_loop_v6

int
multicast_loop_v6::level() noexcept
{
    return native_socket_option::multicast_loop_v6::level();
}
int
multicast_loop_v6::name() noexcept
{
    return native_socket_option::multicast_loop_v6::name();
}

// multicast_hops_v4

int
multicast_hops_v4::level() noexcept
{
    return native_socket_option::multicast_hops_v4::level();
}
int
multicast_hops_v4::name() noexcept
{
    return native_socket_option::multicast_hops_v4::name();
}

// multicast_hops_v6

int
multicast_hops_v6::level() noexcept
{
    return native_socket_option::multicast_hops_v6::level();
}
int
multicast_hops_v6::name() noexcept
{
    return native_socket_option::multicast_hops_v6::name();
}

// multicast_interface_v6

int
multicast_interface_v6::level() noexcept
{
    return native_socket_option::multicast_interface_v6::level();
}
int
multicast_interface_v6::name() noexcept
{
    return native_socket_option::multicast_interface_v6::name();
}

// join_group_v4

join_group_v4::join_group_v4(ipv4_address group, ipv4_address iface) noexcept
{
    native_socket_option::join_group_v4 native(group, iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ip_mreq exceeds join_group_v4 storage");
    std::memcpy(storage_, native.data(), native.size());
}

int
join_group_v4::level() noexcept
{
    return native_socket_option::join_group_v4::level();
}
int
join_group_v4::name() noexcept
{
    return native_socket_option::join_group_v4::name();
}
std::size_t
join_group_v4::size() const noexcept
{
    return native_socket_option::join_group_v4{}.size();
}

// leave_group_v4

leave_group_v4::leave_group_v4(ipv4_address group, ipv4_address iface) noexcept
{
    native_socket_option::leave_group_v4 native(group, iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ip_mreq exceeds leave_group_v4 storage");
    std::memcpy(storage_, native.data(), native.size());
}

int
leave_group_v4::level() noexcept
{
    return native_socket_option::leave_group_v4::level();
}
int
leave_group_v4::name() noexcept
{
    return native_socket_option::leave_group_v4::name();
}
std::size_t
leave_group_v4::size() const noexcept
{
    return native_socket_option::leave_group_v4{}.size();
}

// join_group_v6

join_group_v6::join_group_v6(ipv6_address group, unsigned int if_index) noexcept
{
    native_socket_option::join_group_v6 native(group, if_index);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ipv6_mreq exceeds join_group_v6 storage");
    std::memcpy(storage_, native.data(), native.size());
}

int
join_group_v6::level() noexcept
{
    return native_socket_option::join_group_v6::level();
}
int
join_group_v6::name() noexcept
{
    return native_socket_option::join_group_v6::name();
}
std::size_t
join_group_v6::size() const noexcept
{
    return native_socket_option::join_group_v6{}.size();
}

// leave_group_v6

leave_group_v6::leave_group_v6(
    ipv6_address group, unsigned int if_index) noexcept
{
    native_socket_option::leave_group_v6 native(group, if_index);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ipv6_mreq exceeds leave_group_v6 storage");
    std::memcpy(storage_, native.data(), native.size());
}

int
leave_group_v6::level() noexcept
{
    return native_socket_option::leave_group_v6::level();
}
int
leave_group_v6::name() noexcept
{
    return native_socket_option::leave_group_v6::name();
}
std::size_t
leave_group_v6::size() const noexcept
{
    return native_socket_option::leave_group_v6{}.size();
}

// multicast_interface_v4

multicast_interface_v4::multicast_interface_v4(ipv4_address iface) noexcept
{
    native_socket_option::multicast_interface_v4 native(iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform in_addr exceeds multicast_interface_v4 storage");
    std::memcpy(storage_, native.data(), native.size());
}

int
multicast_interface_v4::level() noexcept
{
    return native_socket_option::multicast_interface_v4::level();
}
int
multicast_interface_v4::name() noexcept
{
    return native_socket_option::multicast_interface_v4::name();
}
std::size_t
multicast_interface_v4::size() const noexcept
{
    return native_socket_option::multicast_interface_v4{}.size();
}

} // namespace boost::corosio::socket_option
