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
no_delay::level(family f) const noexcept
{
    return native_socket_option::no_delay{}.level(f);
}
int
no_delay::name(family f) const noexcept
{
    return native_socket_option::no_delay{}.name(f);
}

// keep_alive

int
keep_alive::level(family f) const noexcept
{
    return native_socket_option::keep_alive{}.level(f);
}
int
keep_alive::name(family f) const noexcept
{
    return native_socket_option::keep_alive{}.name(f);
}

// v6_only

int
v6_only::level(family f) const noexcept
{
    return native_socket_option::v6_only{}.level(f);
}
int
v6_only::name(family f) const noexcept
{
    return native_socket_option::v6_only{}.name(f);
}

// reuse_address

int
reuse_address::level(family f) const noexcept
{
    return native_socket_option::reuse_address{}.level(f);
}
int
reuse_address::name(family f) const noexcept
{
    return native_socket_option::reuse_address{}.name(f);
}

// broadcast

int
broadcast::level(family f) const noexcept
{
    return native_socket_option::broadcast{}.level(f);
}
int
broadcast::name(family f) const noexcept
{
    return native_socket_option::broadcast{}.name(f);
}

// reuse_port

#ifdef SO_REUSEPORT
int
reuse_port::level(family f) const noexcept
{
    return native_socket_option::reuse_port{}.level(f);
}
int
reuse_port::name(family f) const noexcept
{
    return native_socket_option::reuse_port{}.name(f);
}
#else
int
reuse_port::level(family) const noexcept
{
    return SOL_SOCKET;
}
int
reuse_port::name(family) const noexcept
{
    return -1;
}
#endif

// receive_buffer_size

int
receive_buffer_size::level(family f) const noexcept
{
    return native_socket_option::receive_buffer_size{}.level(f);
}
int
receive_buffer_size::name(family f) const noexcept
{
    return native_socket_option::receive_buffer_size{}.name(f);
}

// send_buffer_size

int
send_buffer_size::level(family f) const noexcept
{
    return native_socket_option::send_buffer_size{}.level(f);
}
int
send_buffer_size::name(family f) const noexcept
{
    return native_socket_option::send_buffer_size{}.name(f);
}

// linger

linger::linger(bool enabled, int timeout) noexcept
{
    native_socket_option::linger native(enabled, timeout);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform linger exceeds socket_option::linger storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

bool
linger::enabled() const noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(family::v4), storage_, native.size(family::v4));
    return native.enabled();
}

void
linger::enabled(bool e) noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(family::v4), storage_, native.size(family::v4));
    native.enabled(e);
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
linger::timeout() const noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(family::v4), storage_, native.size(family::v4));
    return native.timeout();
}

void
linger::timeout(int t) noexcept
{
    native_socket_option::linger native;
    std::memcpy(native.data(family::v4), storage_, native.size(family::v4));
    native.timeout(t);
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
linger::level(family f) const noexcept
{
    return native_socket_option::linger{}.level(f);
}
int
linger::name(family f) const noexcept
{
    return native_socket_option::linger{}.name(f);
}

std::size_t
linger::size(family f) const noexcept
{
    return native_socket_option::linger{}.size(f);
}

// multicast_loop
int
multicast_loop::level(family f) const noexcept
{
    return native_socket_option::multicast_loop{}.level(f);
}
int
multicast_loop::name(family f) const noexcept
{
    return native_socket_option::multicast_loop{}.name(f);
}

// multicast_hops
int
multicast_hops::level(family f) const noexcept
{
    return native_socket_option::multicast_hops{}.level(f);
}
int
multicast_hops::name(family f) const noexcept
{
    return native_socket_option::multicast_hops{}.name(f);
}

// multicast_interface_v6

int
multicast_interface_v6::level(family f) const noexcept
{
    return native_socket_option::multicast_interface_v6{}.level(f);
}
int
multicast_interface_v6::name(family f) const noexcept
{
    return native_socket_option::multicast_interface_v6{}.name(f);
}

// join_group_v4

join_group_v4::join_group_v4(ipv4_address group, ipv4_address iface) noexcept
{
    native_socket_option::join_group_v4 native(group, iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ip_mreq exceeds join_group_v4 storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
join_group_v4::level(family f) const noexcept
{
    return native_socket_option::join_group_v4{}.level(f);
}
int
join_group_v4::name(family f) const noexcept
{
    return native_socket_option::join_group_v4{}.name(f);
}
std::size_t
join_group_v4::size(family f) const noexcept
{
    return native_socket_option::join_group_v4{}.size(f);
}

// leave_group_v4

leave_group_v4::leave_group_v4(ipv4_address group, ipv4_address iface) noexcept
{
    native_socket_option::leave_group_v4 native(group, iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ip_mreq exceeds leave_group_v4 storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
leave_group_v4::level(family f) const noexcept
{
    return native_socket_option::leave_group_v4{}.level(f);
}
int
leave_group_v4::name(family f) const noexcept
{
    return native_socket_option::leave_group_v4{}.name(f);
}
std::size_t
leave_group_v4::size(family f) const noexcept
{
    return native_socket_option::leave_group_v4{}.size(f);
}

// join_group_v6

join_group_v6::join_group_v6(ipv6_address group, unsigned int if_index) noexcept
{
    native_socket_option::join_group_v6 native(group, if_index);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ipv6_mreq exceeds join_group_v6 storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
join_group_v6::level(family f) const noexcept
{
    return native_socket_option::join_group_v6{}.level(f);
}
int
join_group_v6::name(family f) const noexcept
{
    return native_socket_option::join_group_v6{}.name(f);
}
std::size_t
join_group_v6::size(family f) const noexcept
{
    return native_socket_option::join_group_v6{}.size(f);
}

// leave_group_v6

leave_group_v6::leave_group_v6(
    ipv6_address group, unsigned int if_index) noexcept
{
    native_socket_option::leave_group_v6 native(group, if_index);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform ipv6_mreq exceeds leave_group_v6 storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
leave_group_v6::level(family f) const noexcept
{
    return native_socket_option::leave_group_v6{}.level(f);
}
int
leave_group_v6::name(family f) const noexcept
{
    return native_socket_option::leave_group_v6{}.name(f);
}
std::size_t
leave_group_v6::size(family f) const noexcept
{
    return native_socket_option::leave_group_v6{}.size(f);
}

// multicast_interface_v4

multicast_interface_v4::multicast_interface_v4(ipv4_address iface) noexcept
{
    native_socket_option::multicast_interface_v4 native(iface);
    static_assert(
        sizeof(native) <= sizeof(storage_),
        "platform in_addr exceeds multicast_interface_v4 storage");
    std::memcpy(storage_, native.data(family::v4), native.size(family::v4));
}

int
multicast_interface_v4::level(family f) const noexcept
{
    return native_socket_option::multicast_interface_v4{}.level(f);
}
int
multicast_interface_v4::name(family f) const noexcept
{
    return native_socket_option::multicast_interface_v4{}.name(f);
}
std::size_t
multicast_interface_v4::size(family f) const noexcept
{
    return native_socket_option::multicast_interface_v4{}.size(f);
}

} // namespace boost::corosio::socket_option
