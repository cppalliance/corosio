//
// Copyright (c) 2026 Steve Gerbino
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
    @code
    sock.set_option( native_socket_option::no_delay( true ) );
    auto nd = sock.get_option<native_socket_option::no_delay>();
    if ( nd.value() )
        // Nagle's algorithm is disabled
    @endcode

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
    static constexpr int level() noexcept
    {
        return Level;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return Name;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt` returns fewer bytes than expected.

        Windows Vista+ may write only 1 byte for boolean options.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(std::size_t s) noexcept
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
    @code
    sock.set_option( native_socket_option::receive_buffer_size( 65536 ) );
    auto opt = sock.get_option<native_socket_option::receive_buffer_size>();
    int sz = opt.value();
    @endcode

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
    static constexpr int level() noexcept
    {
        return Level;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return Name;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt` returns fewer bytes than expected.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(std::size_t s) noexcept
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

    bool value() const noexcept { return value_ != 0; }
    explicit operator bool() const noexcept { return value_ != 0; }
    bool operator!() const noexcept { return value_ == 0; }

    static constexpr int level() noexcept { return Level; }
    static constexpr int name() noexcept { return Name; }

    void* data() noexcept { return &value_; }
    void const* data() const noexcept { return &value_; }
    std::size_t size() const noexcept { return sizeof(value_); }

    void resize(std::size_t) noexcept {}
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
    {}

    byte_integer& operator=(int v) noexcept
    {
        value_ = static_cast<unsigned char>(v);
        return *this;
    }

    int value() const noexcept { return value_; }

    static constexpr int level() noexcept { return Level; }
    static constexpr int name() noexcept { return Name; }

    void* data() noexcept { return &value_; }
    void const* data() const noexcept { return &value_; }
    std::size_t size() const noexcept { return sizeof(value_); }

    void resize(std::size_t) noexcept {}
};

/** The SO_LINGER socket option (native variant).

    Controls behavior when closing a socket with unsent data.
    When enabled, `close()` blocks until pending data is sent
    or the timeout expires.

    This variant stores the platform's `struct linger` directly,
    avoiding the opaque-storage indirection of the type-erased
    version.

    @par Example
    @code
    sock.set_option( native_socket_option::linger( true, 5 ) );
    auto opt = sock.get_option<native_socket_option::linger>();
    if ( opt.enabled() )
        std::cout << "linger timeout: " << opt.timeout() << "s\n";
    @endcode
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
    static constexpr int level() noexcept
    {
        return SOL_SOCKET;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return SO_LINGER;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /** Normalize after `getsockopt`.

        No-op — `struct linger` is always returned at full size.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(std::size_t) noexcept {}
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

/// Enable loopback of outgoing multicast on IPv4 (IP_MULTICAST_LOOP).
using multicast_loop_v4 = byte_boolean<IPPROTO_IP, IP_MULTICAST_LOOP>;

/// Enable loopback of outgoing multicast on IPv6 (IPV6_MULTICAST_LOOP).
using multicast_loop_v6 = boolean<IPPROTO_IPV6, IPV6_MULTICAST_LOOP>;

/// Set the multicast TTL for IPv4 (IP_MULTICAST_TTL).
using multicast_hops_v4 = byte_integer<IPPROTO_IP, IP_MULTICAST_TTL>;

/// Set the multicast hop limit for IPv6 (IPV6_MULTICAST_HOPS).
using multicast_hops_v6 = integer<IPPROTO_IPV6, IPV6_MULTICAST_HOPS>;

/// Set the outgoing interface for IPv6 multicast (IPV6_MULTICAST_IF).
using multicast_interface_v6 = integer<IPPROTO_IPV6, IPV6_MULTICAST_IF>;

/** Join an IPv4 multicast group (IP_ADD_MEMBERSHIP).

    @par Example
    @code
    sock.set_option( native_socket_option::join_group_v4(
        ipv4_address( "239.255.0.1" ) ) );
    @endcode
*/
class join_group_v4
{
    struct ip_mreq value_{};

public:
    /// Construct with default values.
    join_group_v4() = default;

    /** Construct with a group and optional interface address.

        @param group The multicast group address to join.
        @param iface The local interface to use (default: any).
    */
    join_group_v4(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept
    {
        auto gb = group.to_bytes();
        auto ib = iface.to_bytes();
        std::memcpy(&value_.imr_multiaddr, gb.data(), 4);
        std::memcpy(&value_.imr_interface, ib.data(), 4);
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    static constexpr int level() noexcept
    {
        return IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return IP_ADD_MEMBERSHIP;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Leave an IPv4 multicast group (IP_DROP_MEMBERSHIP).

    @par Example
    @code
    sock.set_option( native_socket_option::leave_group_v4(
        ipv4_address( "239.255.0.1" ) ) );
    @endcode
*/
class leave_group_v4
{
    struct ip_mreq value_{};

public:
    /// Construct with default values.
    leave_group_v4() = default;

    /** Construct with a group and optional interface address.

        @param group The multicast group address to leave.
        @param iface The local interface (default: any).
    */
    leave_group_v4(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept
    {
        auto gb = group.to_bytes();
        auto ib = iface.to_bytes();
        std::memcpy(&value_.imr_multiaddr, gb.data(), 4);
        std::memcpy(&value_.imr_interface, ib.data(), 4);
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    static constexpr int level() noexcept
    {
        return IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return IP_DROP_MEMBERSHIP;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Join an IPv6 multicast group (IPV6_JOIN_GROUP).

    @par Example
    @code
    sock.set_option( native_socket_option::join_group_v6(
        ipv6_address( "ff02::1" ), 0 ) );
    @endcode
*/
class join_group_v6
{
    struct ipv6_mreq value_{};

public:
    /// Construct with default values.
    join_group_v6() = default;

    /** Construct with a group and optional interface index.

        @param group The multicast group address to join.
        @param if_index The interface index (0 = kernel chooses).
    */
    join_group_v6(ipv6_address group, unsigned int if_index = 0) noexcept
    {
        auto gb = group.to_bytes();
        std::memcpy(&value_.ipv6mr_multiaddr, gb.data(), 16);
        value_.ipv6mr_interface = if_index;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    static constexpr int level() noexcept
    {
        return IPPROTO_IPV6;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return IPV6_JOIN_GROUP;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Leave an IPv6 multicast group (IPV6_LEAVE_GROUP).

    @par Example
    @code
    sock.set_option( native_socket_option::leave_group_v6(
        ipv6_address( "ff02::1" ), 0 ) );
    @endcode
*/
class leave_group_v6
{
    struct ipv6_mreq value_{};

public:
    /// Construct with default values.
    leave_group_v6() = default;

    /** Construct with a group and optional interface index.

        @param group The multicast group address to leave.
        @param if_index The interface index (0 = kernel chooses).
    */
    leave_group_v6(ipv6_address group, unsigned int if_index = 0) noexcept
    {
        auto gb = group.to_bytes();
        std::memcpy(&value_.ipv6mr_multiaddr, gb.data(), 16);
        value_.ipv6mr_interface = if_index;
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    static constexpr int level() noexcept
    {
        return IPPROTO_IPV6;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return IPV6_LEAVE_GROUP;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Set the outgoing interface for IPv4 multicast (IP_MULTICAST_IF).

    Unlike the integer-based `multicast_interface_v6`, this option
    takes an `ipv4_address` identifying the local interface.

    @par Example
    @code
    sock.set_option( native_socket_option::multicast_interface_v4(
        ipv4_address( "192.168.1.1" ) ) );
    @endcode
*/
class multicast_interface_v4
{
    struct in_addr value_{};

public:
    /// Construct with default values (INADDR_ANY).
    multicast_interface_v4() = default;

    /** Construct with an interface address.

        @param iface The local interface address.
    */
    explicit multicast_interface_v4(ipv4_address iface) noexcept
    {
        auto b = iface.to_bytes();
        std::memcpy(&value_, b.data(), 4);
    }

    /// Return the protocol level for `setsockopt`/`getsockopt`.
    static constexpr int level() noexcept
    {
        return IPPROTO_IP;
    }

    /// Return the option name for `setsockopt`/`getsockopt`.
    static constexpr int name() noexcept
    {
        return IP_MULTICAST_IF;
    }

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return &value_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return &value_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept
    {
        return sizeof(value_);
    }

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

} // namespace boost::corosio::native_socket_option

#endif // BOOST_COROSIO_NATIVE_NATIVE_SOCKET_OPTION_HPP
