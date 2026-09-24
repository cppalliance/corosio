//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/** @file native_socket_option.hpp

    Inline socket option types using platform-specific constants.
    All methods are `constexpr` or trivially inlined, giving zero
    overhead compared to hand-written `setsockopt` calls.

    This header includes platform socket headers
    (`<sys/socket.h>`, `<netinet/tcp.h>`, etc.).
    For a version that avoids platform includes, use
    `<boost/corosio/socket_option.hpp>`
    (`boost::corosio::socket_option`).

    Both variants satisfy the same option-type interface and work
    interchangeably with `tcp_socket::set_option` /
    `tcp_socket::get_option` and the corresponding acceptor methods.

    @see boost::corosio::socket_option
*/

#ifndef BOOST_COROSIO_NATIVE_NATIVE_SOCKET_OPTION_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_SOCKET_OPTION_HPP

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

// Some older systems define only the legacy names
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP IPV6_ADD_MEMBERSHIP
#endif
#ifndef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/family.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include <cstddef>
#include <cstring>

namespace boost::corosio::native_socket_option {

/** A socket option with a boolean value.

    Models socket options whose underlying representation is an `int`
    where 0 means disabled and non-zero means enabled. The option's
    protocol level and name are encoded as template parameters.

    This is the native (inline) variant that includes platform
    headers. For a type-erased version that avoids platform
    includes, use `boost::corosio::socket_option` instead.

    @par Example
    @par !example boolean

    @tparam Level The protocol level (e.g. `SOL_SOCKET`, `IPPROTO_TCP`).
    @tparam Name The option name (e.g. `TCP_NODELAY`, `SO_KEEPALIVE`).
*/
template<int Level, int Name>
class boolean
{
    int value_ = 0;

public:
    /// Construct with default value (disabled).
    boolean() = default;

    /** Construct with an explicit value.

        @param v `true` to enable the option, `false` to disable.
    */
    explicit boolean(bool v) noexcept : value_(v ? 1 : 0) {}

    /// Assign a new value.
    boolean& operator=(bool v) noexcept
    {
        value_ = v ? 1 : 0;
        return *this;
    }

    /// Return the option value.
    bool value() const noexcept
    {
        return value_ != 0;
    }

    /// Return the option value.
    explicit operator bool() const noexcept
    {
        return value_ != 0;
    }

    /// Return the negated option value.
    bool operator!() const noexcept
    {
        return value_ == 0;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family) const noexcept
    {
        return Level;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family) const noexcept
    {
        return Name;
    }

    /// Return a pointer to the underlying storage.
    void* data(family) noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt` returns fewer bytes than expected.

        Windows Vista+ may write only 1 byte for boolean options.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(family, std::size_t s) noexcept
    {
        if (s == sizeof(char))
            value_ = *reinterpret_cast<unsigned char*>(&value_) ? 1 : 0;
    }
};

/** A socket option with an integer value.

    Models socket options whose underlying representation is a
    plain `int`. The option's protocol level and name are encoded
    as template parameters.

    This is the native (inline) variant that includes platform
    headers. For a type-erased version that avoids platform
    includes, use `boost::corosio::socket_option` instead.

    @par Example
    @par !example integer

    @tparam Level The protocol level (e.g. `SOL_SOCKET`).
    @tparam Name The option name (e.g. `SO_RCVBUF`).
*/
template<int Level, int Name>
class integer
{
    int value_ = 0;

public:
    /// Construct with default value (zero).
    integer() = default;

    /** Construct with an explicit value.

        @param v The option value.
    */
    explicit integer(int v) noexcept : value_(v) {}

    /// Assign a new value.
    integer& operator=(int v) noexcept
    {
        value_ = v;
        return *this;
    }

    /// Return the option value.
    int value() const noexcept
    {
        return value_;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family) const noexcept
    {
        return Level;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family) const noexcept
    {
        return Name;
    }

    /// Return a pointer to the underlying storage.
    void* data(family) noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt` returns fewer bytes than expected.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(family, std::size_t s) noexcept
    {
        if (s == sizeof(char))
            value_ =
                static_cast<int>(*reinterpret_cast<unsigned char*>(&value_));
    }
};

/** A boolean socket option with single-byte storage.

    Some BSD-derived kernels (macOS, FreeBSD) require certain IPv4 multicast
    options (`IP_MULTICAST_LOOP`) to be set with a one-byte value and return
    `EINVAL` for the four-byte form that Linux accepts. This template
    provides `unsigned char` storage so the option works on every platform.

    @tparam Level The protocol level.
    @tparam Name The option name.
*/
template<int Level, int Name>
class byte_boolean
{
    unsigned char value_ = 0;

public:
    byte_boolean() = default;

    explicit byte_boolean(bool v) noexcept : value_(v ? 1 : 0) {}

    byte_boolean& operator=(bool v) noexcept
    {
        value_ = v ? 1 : 0;
        return *this;
    }

    bool value() const noexcept
    {
        return value_ != 0;
    }
    explicit operator bool() const noexcept
    {
        return value_ != 0;
    }
    bool operator!() const noexcept
    {
        return value_ == 0;
    }

    constexpr int level(family) const noexcept
    {
        return Level;
    }
    constexpr int name(family) const noexcept
    {
        return Name;
    }

    void* data(family) noexcept
    {
        return &value_;
    }
    void const* data(family) const noexcept
    {
        return &value_;
    }
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }

    void resize(family, std::size_t) noexcept {}
};

/** An integer socket option with single-byte storage.

    Same rationale as `byte_boolean`: BSD-derived kernels require
    `IP_MULTICAST_TTL` to be set with a one-byte value. Linux accepts
    one byte too, so single-byte storage is portable. Values are
    truncated to the 0–255 range.

    @tparam Level The protocol level.
    @tparam Name The option name.
*/
template<int Level, int Name>
class byte_integer
{
    unsigned char value_ = 0;

public:
    byte_integer() = default;

    explicit byte_integer(int v) noexcept
        : value_(static_cast<unsigned char>(v))
    {
    }

    byte_integer& operator=(int v) noexcept
    {
        value_ = static_cast<unsigned char>(v);
        return *this;
    }

    int value() const noexcept
    {
        return value_;
    }

    constexpr int level(family) const noexcept
    {
        return Level;
    }
    constexpr int name(family) const noexcept
    {
        return Name;
    }

    void* data(family) noexcept
    {
        return &value_;
    }
    void const* data(family) const noexcept
    {
        return &value_;
    }
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }

    void resize(family, std::size_t) noexcept {}
};

/** The SO_LINGER socket option (native variant).

    Controls behavior when closing a socket with unsent data.
    When enabled, `close()` blocks until pending data is sent
    or the timeout expires.

    This variant stores the platform's `struct linger` directly,
    avoiding the opaque-storage indirection of the type-erased
    version.

    @par Example
    @par !example linger
*/
class linger
{
    struct ::linger value_{};

public:
    /// Construct with default values (disabled, zero timeout).
    linger() = default;

    /** Construct with explicit values.

        @param enabled `true` to enable linger behavior on close.
        @param timeout The linger timeout in seconds.
    */
    linger(bool enabled, int timeout) noexcept
    {
        value_.l_onoff  = enabled ? 1 : 0;
        value_.l_linger = static_cast<decltype(value_.l_linger)>(timeout);
    }

    /// Return whether linger is enabled.
    bool enabled() const noexcept
    {
        return value_.l_onoff != 0;
    }

    /// Set whether linger is enabled.
    void enabled(bool v) noexcept
    {
        value_.l_onoff = v ? 1 : 0;
    }

    /// Return the linger timeout in seconds.
    int timeout() const noexcept
    {
        return static_cast<int>(value_.l_linger);
    }

    /// Set the linger timeout in seconds.
    void timeout(int v) noexcept
    {
        value_.l_linger = static_cast<decltype(value_.l_linger)>(v);
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family) const noexcept
    {
        return SOL_SOCKET;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family) const noexcept
    {
        return SO_LINGER;
    }

    /// Return a pointer to the underlying storage.
    void* data(family) noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size(family) const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt`.

        No-op — `struct linger` is always returned at full size.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(family, std::size_t) noexcept {}
};

/// Disable Nagle's algorithm (TCP_NODELAY).
using no_delay = boolean<IPPROTO_TCP, TCP_NODELAY>;

/// Enable periodic keepalive probes (SO_KEEPALIVE).
using keep_alive = boolean<SOL_SOCKET, SO_KEEPALIVE>;

/// Restrict an IPv6 socket to IPv6 only (IPV6_V6ONLY).
using v6_only = boolean<IPPROTO_IPV6, IPV6_V6ONLY>;

/// Allow local address reuse (SO_REUSEADDR).
using reuse_address = boolean<SOL_SOCKET, SO_REUSEADDR>;

/// Allow sending to broadcast addresses (SO_BROADCAST).
using broadcast = boolean<SOL_SOCKET, SO_BROADCAST>;

/// Set the receive buffer size (SO_RCVBUF).
using receive_buffer_size = integer<SOL_SOCKET, SO_RCVBUF>;

/// Set the send buffer size (SO_SNDBUF).
using send_buffer_size = integer<SOL_SOCKET, SO_SNDBUF>;

#ifdef SO_REUSEPORT
/// Allow multiple sockets to bind to the same port (SO_REUSEPORT).
using reuse_port = boolean<SOL_SOCKET, SO_REUSEPORT>;
#endif

/** Enable loopback of outgoing multicast (IP_MULTICAST_LOOP /
    IPV6_MULTICAST_LOOP).

    The socket's family selects the wire rendering: a single byte
    at `IPPROTO_IP` for IPv4 (BSD-derived kernels reject the
    four-byte form), an `int` at `IPPROTO_IPV6` for IPv6.
*/
class multicast_loop
{
    unsigned char byte_ = 0; // IPv4 rendering
    int int_            = 0; // IPv6 rendering

public:
    /// Construct with default value (disabled).
    multicast_loop() = default;

    /** Construct with an explicit value.

        @param v `true` to enable loopback, `false` to disable.
    */
    explicit multicast_loop(bool v) noexcept : byte_(v ? 1 : 0), int_(v ? 1 : 0)
    {
    }

    /// Assign a new value.
    multicast_loop& operator=(bool v) noexcept
    {
        byte_ = v ? 1 : 0;
        int_  = v ? 1 : 0;
        return *this;
    }

    /// Return the option value.
    bool value() const noexcept
    {
        return byte_ != 0 || int_ != 0;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family f) const noexcept
    {
        return f == family::v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family f) const noexcept
    {
        return f == family::v6 ? IPV6_MULTICAST_LOOP : IP_MULTICAST_LOOP;
    }

    /// Return a pointer to the rendering for `f`.
    void* data(family f) noexcept
    {
        return f == family::v6 ? static_cast<void*>(&int_)
                               : static_cast<void*>(&byte_);
    }

    /// Return a pointer to the rendering for `f`.
    void const* data(family f) const noexcept
    {
        return f == family::v6 ? static_cast<void const*>(&int_)
                               : static_cast<void const*>(&byte_);
    }

    /// Return the size of the rendering for `f`.
    std::size_t size(family f) const noexcept
    {
        return f == family::v6 ? sizeof(int_) : sizeof(byte_);
    }

    /** Synchronize both renderings after `getsockopt`.

        Only the rendering the socket's family selected was
        written; fold it into the other so `value()` answers
        from either.

        @param f The family `getsockopt` was performed for.
    */
    void resize(family f, std::size_t) noexcept
    {
        if (f == family::v6)
            byte_ = int_ ? 1 : 0;
        else
            int_ = byte_ ? 1 : 0;
    }
};

/** Set the multicast TTL / hop limit (IP_MULTICAST_TTL /
    IPV6_MULTICAST_HOPS).

    The socket's family selects the wire rendering: a single byte
    at `IPPROTO_IP` for IPv4, an `int` at `IPPROTO_IPV6` for IPv6.
*/
class multicast_hops
{
    unsigned char byte_ = 0; // IPv4 rendering
    int int_            = 0; // IPv6 rendering

public:
    /// Construct with default value (zero).
    multicast_hops() = default;

    /** Construct with an explicit value.

        @param v The hop count, 0 to 255 — the range the IPv4 wire
        rendering can carry.

        @throws std::logic_error if `v` is outside [0, 255].
    */
    explicit multicast_hops(int v)
    {
        if (v < 0 || v > 255)
            detail::throw_logic_error("multicast hops value out of range");
        byte_ = static_cast<unsigned char>(v);
        int_  = v;
    }

    /** Assign a new value.

        @throws std::logic_error if `v` is outside [0, 255].
    */
    multicast_hops& operator=(int v)
    {
        if (v < 0 || v > 255)
            detail::throw_logic_error("multicast hops value out of range");
        byte_ = static_cast<unsigned char>(v);
        int_  = v;
        return *this;
    }

    /// Return the option value.
    int value() const noexcept
    {
        return int_;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family f) const noexcept
    {
        return f == family::v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family f) const noexcept
    {
        return f == family::v6 ? IPV6_MULTICAST_HOPS : IP_MULTICAST_TTL;
    }

    /// Return a pointer to the rendering for `f`.
    void* data(family f) noexcept
    {
        return f == family::v6 ? static_cast<void*>(&int_)
                               : static_cast<void*>(&byte_);
    }

    /// Return a pointer to the rendering for `f`.
    void const* data(family f) const noexcept
    {
        return f == family::v6 ? static_cast<void const*>(&int_)
                               : static_cast<void const*>(&byte_);
    }

    /// Return the size of the rendering for `f`.
    std::size_t size(family f) const noexcept
    {
        return f == family::v6 ? sizeof(int_) : sizeof(byte_);
    }

    /** Synchronize both renderings after `getsockopt`.

        @param f The family `getsockopt` was performed for.
    */
    void resize(family f, std::size_t) noexcept
    {
        if (f == family::v6)
            byte_ = static_cast<unsigned char>(int_);
        else
            int_ = byte_;
    }
};

/** A multicast membership request.

    The group's family — not the socket's — selects the wire
    struct and protocol level: a v4 group renders as an `ip_mreq`
    at the IPv4 level even when applied to a dual-stack v6 socket,
    which is the level such a join actually targets.

    @tparam Level4 The IPv4 protocol level.
    @tparam Name4 The IPv4 option name.
    @tparam Level6 The IPv6 protocol level.
    @tparam Name6 The IPv6 option name.
*/
template<int Level4, int Name4, int Level6, int Name6>
class membership_request
{
    struct ip_mreq v4_{};
    struct ipv6_mreq v6_{};
    bool is_v4_ = true;

    void assign_v4(ipv4_address group, ipv4_address iface) noexcept
    {
        auto g = group.to_bytes();
        std::memcpy(&v4_.imr_multiaddr, g.data(), 4);
        auto i = iface.to_bytes();
        std::memcpy(&v4_.imr_interface, i.data(), 4);
        is_v4_ = true;
    }

    void assign_v6(ipv6_address const& group, unsigned int if_index) noexcept
    {
        auto g = group.to_bytes();
        std::memcpy(&v6_.ipv6mr_multiaddr, g.data(), 16);
        // The group's zone is the natural default interface
        v6_.ipv6mr_interface = if_index ? if_index : group.scope_id();
        is_v4_               = false;
    }

public:
    /// Construct with default values.
    membership_request() = default;

    /** Construct from a group address.

        The group's family selects the wire representation; the
        interface defaults to any (v4) or the group's zone (v6).

        @param group The multicast group address.
    */
    explicit membership_request(ip_address const& group) noexcept
    {
        if (group.is_v4())
            assign_v4(group.to_v4(), ipv4_address());
        else
            assign_v6(group.to_v6(), 0);
    }

    /** Construct from an IPv4 group and interface address.

        @param group The multicast group address.
        @param iface The local interface to use (default: any).
    */
    membership_request(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept
    {
        assign_v4(group, iface);
    }

    /** Construct from an IPv6 group and interface index.

        @param group The multicast group address.
        @param if_index The interface index; 0 uses the group's
        zone, and a zone of 0 lets the kernel choose.
    */
    membership_request(
        ipv6_address const& group, unsigned int if_index = 0) noexcept
    {
        assign_v6(group, if_index);
    }

    /// Return the protocol level for the group's family.
    constexpr int level(family) const noexcept
    {
        return is_v4_ ? Level4 : Level6;
    }

    /// Return the option name for the group's family.
    constexpr int name(family) const noexcept
    {
        return is_v4_ ? Name4 : Name6;
    }

    /// Return a pointer to the wire struct for the group's family.
    void const* data(family) const noexcept
    {
        return is_v4_ ? static_cast<void const*>(&v4_)
                      : static_cast<void const*>(&v6_);
    }

    /// Return the size of the wire struct for the group's family.
    std::size_t size(family) const noexcept
    {
        return is_v4_ ? sizeof(v4_) : sizeof(v6_);
    }

    /// No-op resize.
    void resize(family, std::size_t) noexcept {}
};

/// Join a multicast group (IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP).
using join_group = membership_request<
    IPPROTO_IP,
    IP_ADD_MEMBERSHIP,
    IPPROTO_IPV6,
    IPV6_JOIN_GROUP>;

/// Leave a multicast group (IP_DROP_MEMBERSHIP / IPV6_LEAVE_GROUP).
using leave_group = membership_request<
    IPPROTO_IP,
    IP_DROP_MEMBERSHIP,
    IPPROTO_IPV6,
    IPV6_LEAVE_GROUP>;

/** Set the outgoing multicast interface (IP_MULTICAST_IF /
    IPV6_MULTICAST_IF).

    The two families name interfaces differently on the wire — IPv4
    by interface address, IPv6 by interface index — so the option
    stores both renderings and the socket's family selects one; the
    other stays at its default (any address, kernel-chosen index).
*/
class multicast_interface
{
    struct in_addr v4_{};
    unsigned int if_index_ = 0;

public:
    /// Construct with default values (any address, kernel-chosen index).
    multicast_interface() = default;

    /** Construct with an IPv4 interface address.

        @param iface The local interface address.
    */
    explicit multicast_interface(ipv4_address iface) noexcept
    {
        auto b = iface.to_bytes();
        std::memcpy(&v4_, b.data(), 4);
    }

    /** Construct with an IPv6 interface index.

        @param if_index The interface index (0 = kernel chooses).
    */
    explicit multicast_interface(unsigned int if_index) noexcept
        : if_index_(if_index)
    {
    }

    /// Return the IPv4 rendering as an address.
    ipv4_address address() const noexcept
    {
        ipv4_address::bytes_type b;
        std::memcpy(b.data(), &v4_, 4);
        return ipv4_address(b);
    }

    /// Return the IPv6 rendering as an interface index.
    unsigned int if_index() const noexcept
    {
        return if_index_;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    constexpr int level(family f) const noexcept
    {
        return f == family::v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    constexpr int name(family f) const noexcept
    {
        return f == family::v6 ? IPV6_MULTICAST_IF : IP_MULTICAST_IF;
    }

    /// Return a pointer to the rendering for `f`.
    void* data(family f) noexcept
    {
        return f == family::v6 ? static_cast<void*>(&if_index_)
                               : static_cast<void*>(&v4_);
    }

    /// Return a pointer to the rendering for `f`.
    void const* data(family f) const noexcept
    {
        return f == family::v6 ? static_cast<void const*>(&if_index_)
                               : static_cast<void const*>(&v4_);
    }

    /// Return the size of the rendering for `f`.
    std::size_t size(family f) const noexcept
    {
        return f == family::v6 ? sizeof(if_index_) : sizeof(v4_);
    }

    /// No-op resize.
    void resize(family, std::size_t) noexcept {}
};

} // namespace boost::corosio::native_socket_option

#endif // BOOST_COROSIO_NATIVE_NATIVE_SOCKET_OPTION_HPP
