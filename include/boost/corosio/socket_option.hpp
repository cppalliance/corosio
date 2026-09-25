//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SOCKET_OPTION_HPP
#define BOOST_COROSIO_SOCKET_OPTION_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/family.hpp>
#include <boost/corosio/ip_address.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include <cstddef>

/** @file socket_option.hpp

    Type-erased socket option types that avoid platform-specific
    headers. The protocol level and option name for each type are
    resolved at link time via the compiled library.

    For an inline (zero-overhead) alternative that includes platform
    headers, use `<boost/corosio/native/native_socket_option.hpp>`
    (`boost::corosio::native_socket_option`).

    Both variants satisfy the same option-type interface and work
    interchangeably with `tcp_socket::set_option` /
    `tcp_socket::get_option` and the corresponding acceptor methods.

    @see native_socket_option
*/

namespace boost::corosio::socket_option {

/** Base class for concrete boolean socket options.

    Stores a boolean as an `int` suitable for `setsockopt`/`getsockopt`.
    Derived types provide `level()` and `name()` for the specific option.
*/
class BOOST_COROSIO_DECL boolean_option
{
    int value_ = 0;

public:
    /// Construct with default value (disabled).
    boolean_option() = default;

    /** Construct with an explicit value.

        @param v `true` to enable the option, `false` to disable.
    */
    explicit boolean_option(bool v) noexcept : value_(v ? 1 : 0) {}

    /// Assign a new value.
    boolean_option& operator=(bool v) noexcept
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

/** Base class for concrete integer socket options.

    Stores an integer suitable for `setsockopt`/`getsockopt`.
    Derived types provide `level()` and `name()` for the specific option.
*/
class BOOST_COROSIO_DECL integer_option
{
    int value_ = 0;

public:
    /// Construct with default value (zero).
    integer_option() = default;

    /** Construct with an explicit value.

        @param v The option value.
    */
    explicit integer_option(int v) noexcept : value_(v) {}

    /// Assign a new value.
    integer_option& operator=(int v) noexcept
    {
        value_ = v;
        return *this;
    }

    /// Return the option value.
    int value() const noexcept
    {
        return value_;
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

/** Disable Nagle's algorithm (TCP_NODELAY).

    @par Example
    @par !example no_delay
*/
class BOOST_COROSIO_DECL no_delay : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Enable periodic keepalive probes (SO_KEEPALIVE).

    @par Example
    @par !example keep_alive
*/
class BOOST_COROSIO_DECL keep_alive : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Restrict an IPv6 socket to IPv6 only (IPV6_V6ONLY).

    When enabled, the socket only accepts IPv6 connections.
    When disabled, the socket accepts both IPv4 and IPv6
    connections (dual-stack mode).

    @par Example
    @par !example v6_only
*/
class BOOST_COROSIO_DECL v6_only : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Allow local address reuse (SO_REUSEADDR).

    @par Example
    @par !example reuse_address
*/
class BOOST_COROSIO_DECL reuse_address : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Allow sending to broadcast addresses (SO_BROADCAST).

    Required for UDP sockets that send to broadcast addresses
    such as 255.255.255.255. Without this option, `send_to`
    returns an error.

    @par Example
    @par !example broadcast
*/
class BOOST_COROSIO_DECL broadcast : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Allow multiple sockets to bind to the same port (SO_REUSEPORT).

    Not available on all platforms. On unsupported platforms,
    `set_option` throws `std::system_error`.

    @par Example
    @par !example reuse_port
*/
class BOOST_COROSIO_DECL reuse_port : public boolean_option
{
public:
    /// Inherit the base constructors.
    using boolean_option::boolean_option;

    /// Inherit assignment from the base.
    using boolean_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Set the receive buffer size (SO_RCVBUF).

    @par Example
    @par !example receive_buffer_size
*/
class BOOST_COROSIO_DECL receive_buffer_size : public integer_option
{
public:
    /// Inherit the base constructors.
    using integer_option::integer_option;

    /// Inherit assignment from the base.
    using integer_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** Set the send buffer size (SO_SNDBUF).

    @par Example
    @par !example send_buffer_size
*/
class BOOST_COROSIO_DECL send_buffer_size : public integer_option
{
public:
    /// Inherit the base constructors.
    using integer_option::integer_option;

    /// Inherit assignment from the base.
    using integer_option::operator=;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;
};

/** The SO_LINGER socket option.

    Controls behavior when closing a socket with unsent data.
    When enabled, `close()` blocks until pending data is sent
    or the timeout expires.

    @par Example
    @par !example linger
*/
class BOOST_COROSIO_DECL linger
{
    // Opaque storage for the platform's struct linger.
    // POSIX: { int, int } = 8 bytes.
    // Windows: { u_short, u_short } = 4 bytes.
    static constexpr std::size_t max_storage_ = 8;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values (disabled, zero timeout).
    linger() noexcept = default;

    /** Construct with explicit values.

        @param enabled `true` to enable linger behavior on close.
        @param timeout The linger timeout in seconds.
    */
    linger(bool enabled, int timeout) noexcept;

    /// Return whether linger is enabled.
    bool enabled() const noexcept;

    /** Set whether linger is enabled.

        @param v `true` to linger on close.
    */
    void enabled(bool v) noexcept;

    /// Return the linger timeout in seconds.
    int timeout() const noexcept;

    /** Set the linger timeout in seconds.

        @param v The timeout in seconds.
    */
    void timeout(int v) noexcept;

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;

    /// Return a pointer to the underlying storage.
    void* data(family) noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size(family) const noexcept;

    /** Normalize after `getsockopt`.

        No-op — `struct linger` is always returned at full size.
    */
    void resize(family, std::size_t) noexcept {}
};

/** Enable loopback of outgoing multicast (IP_MULTICAST_LOOP /
    IPV6_MULTICAST_LOOP).

    The socket's family selects the wire rendering. A single byte
    at the IPv4 level (BSD-derived kernels reject the four-byte
    form), an `int` at the IPv6 level.

    @par Example
    @par !example multicast_loop
*/
class BOOST_COROSIO_DECL multicast_loop
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
        return int_ != 0;
    }

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;

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

        Only the rendering the socket's family selected was written;
        fold it into the other so `value()` answers from either.

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
    at the IPv4 level, an `int` at the IPv6 level.

    @par Example
    @par !example multicast_hops
*/
class BOOST_COROSIO_DECL multicast_hops
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

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;

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

/** Join a multicast group (IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP).

    The group's family — not the socket's — selects the wire struct and
    protocol level. A v4 group renders as an `ip_mreq` at the IPv4 level
    even when applied to a dual-stack v6 socket. That is the level such a
    join actually targets.

    @par Example
    @par !example join_group
*/
class BOOST_COROSIO_DECL join_group
{
    // Opaque storage sized for the larger of ip_mreq / ipv6_mreq
    static constexpr std::size_t max_storage_ = 20;
    alignas(4) unsigned char storage_[max_storage_]{};
    family group_family_ = family::v4;

public:
    /// Construct with default values.
    join_group() noexcept = default;

    /** Construct from a group address.

        The group's family selects the wire representation; the
        interface defaults to any (v4) or the group's zone (v6).

        @param group The multicast group address to join.
    */
    explicit join_group(ip_address const& group) noexcept;

    /** Construct from an IPv4 group and interface address.

        @param group The multicast group address to join.
        @param iface The local interface to use (default: any).
    */
    join_group(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept;

    /** Construct from an IPv6 group and interface index.

        @param group The multicast group address to join.
        @param if_index The interface index; 0 uses the group's
        zone, and a zone of 0 lets the kernel choose.
    */
    join_group(ipv6_address const& group, unsigned int if_index = 0) noexcept;

    /// Return the protocol level for the group's family.
    int level(family) const noexcept;

    /// Return the option name for the group's family.
    int name(family) const noexcept;

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return storage_;
    }

    /// Return the size of the wire struct for the group's family.
    std::size_t size(family) const noexcept;

    /// No-op resize.
    void resize(family, std::size_t) noexcept {}
};

/** Leave a multicast group (IP_DROP_MEMBERSHIP / IPV6_LEAVE_GROUP).

    The group's family — not the socket's — selects the wire
    struct and protocol level, mirroring @ref join_group.

    @par Example
    @par !example leave_group
*/
class BOOST_COROSIO_DECL leave_group
{
    static constexpr std::size_t max_storage_ = 20;
    alignas(4) unsigned char storage_[max_storage_]{};
    family group_family_ = family::v4;

public:
    /// Construct with default values.
    leave_group() noexcept = default;

    /** Construct from a group address.

        @param group The multicast group address to leave.
    */
    explicit leave_group(ip_address const& group) noexcept;

    /** Construct from an IPv4 group and interface address.

        @param group The multicast group address to leave.
        @param iface The local interface (default: any).
    */
    leave_group(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept;

    /** Construct from an IPv6 group and interface index.

        @param group The multicast group address to leave.
        @param if_index The interface index; 0 uses the group's
        zone, and a zone of 0 lets the kernel choose.
    */
    leave_group(ipv6_address const& group, unsigned int if_index = 0) noexcept;

    /// Return the protocol level for the group's family.
    int level(family) const noexcept;

    /// Return the option name for the group's family.
    int name(family) const noexcept;

    /// Return a pointer to the underlying storage.
    void const* data(family) const noexcept
    {
        return storage_;
    }

    /// Return the size of the wire struct for the group's family.
    std::size_t size(family) const noexcept;

    /// No-op resize.
    void resize(family, std::size_t) noexcept {}
};

/** Set the outgoing multicast interface (IP_MULTICAST_IF /
    IPV6_MULTICAST_IF).

    The two families name interfaces differently on the wire: IPv4 by
    interface address, IPv6 by interface index. The option stores both
    renderings and the socket's family selects one; the other stays at its
    default (any address, kernel-chosen index).

    @par Example
    @par !example multicast_interface
*/
class BOOST_COROSIO_DECL multicast_interface
{
    alignas(4) unsigned char v4_storage_[4]{};
    unsigned int if_index_ = 0;

public:
    /// Construct with default values (any address, kernel-chosen index).
    multicast_interface() noexcept = default;

    /** Construct with an IPv4 interface address.

        @param iface The local interface address.
    */
    explicit multicast_interface(ipv4_address iface) noexcept;

    /** Construct with an IPv6 interface index.

        @param if_index The interface index (0 = kernel chooses).
    */
    explicit multicast_interface(unsigned int if_index) noexcept
        : if_index_(if_index)
    {
    }

    /// Return the IPv4 rendering as an address.
    ipv4_address address() const noexcept;

    /// Return the IPv6 rendering as an interface index.
    unsigned int if_index() const noexcept
    {
        return if_index_;
    }

    /// Return the protocol level.
    int level(family) const noexcept;

    /// Return the option name.
    int name(family) const noexcept;

    /// Return a pointer to the rendering for `f`.
    void* data(family f) noexcept
    {
        return f == family::v6 ? static_cast<void*>(&if_index_)
                               : static_cast<void*>(v4_storage_);
    }

    /// Return a pointer to the rendering for `f`.
    void const* data(family f) const noexcept
    {
        return f == family::v6 ? static_cast<void const*>(&if_index_)
                               : static_cast<void const*>(v4_storage_);
    }

    /// Return the size of the rendering for `f`.
    std::size_t size(family) const noexcept;

    /// No-op resize.
    void resize(family, std::size_t) noexcept {}
};

} // namespace boost::corosio::socket_option

#endif // BOOST_COROSIO_SOCKET_OPTION_HPP
