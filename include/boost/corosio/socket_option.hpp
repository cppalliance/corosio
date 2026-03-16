//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SOCKET_OPTION_HPP
#define BOOST_COROSIO_SOCKET_OPTION_HPP

#include <boost/corosio/detail/config.hpp>
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
class boolean_option
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

/** Base class for concrete integer socket options.

    Stores an integer suitable for `setsockopt`/`getsockopt`.
    Derived types provide `level()` and `name()` for the specific option.
*/
class integer_option
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

/** Disable Nagle's algorithm (TCP_NODELAY).

    @par Example
    @code
    sock.set_option( socket_option::no_delay( true ) );
    auto nd = sock.get_option<socket_option::no_delay>();
    if ( nd.value() )
        // Nagle's algorithm is disabled
    @endcode
*/
class BOOST_COROSIO_DECL no_delay : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Enable periodic keepalive probes (SO_KEEPALIVE).

    @par Example
    @code
    sock.set_option( socket_option::keep_alive( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL keep_alive : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Restrict an IPv6 socket to IPv6 only (IPV6_V6ONLY).

    When enabled, the socket only accepts IPv6 connections.
    When disabled, the socket accepts both IPv4 and IPv6
    connections (dual-stack mode).

    @par Example
    @code
    sock.set_option( socket_option::v6_only( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL v6_only : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Allow local address reuse (SO_REUSEADDR).

    @par Example
    @code
    acc.set_option( socket_option::reuse_address( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL reuse_address : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Allow sending to broadcast addresses (SO_BROADCAST).

    Required for UDP sockets that send to broadcast addresses
    such as 255.255.255.255. Without this option, `send_to`
    returns an error.

    @par Example
    @code
    udp_socket sock( ioc );
    sock.open();
    sock.set_option( socket_option::broadcast( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL broadcast : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Allow multiple sockets to bind to the same port (SO_REUSEPORT).

    Not available on all platforms. On unsupported platforms,
    `set_option` will return an error.

    @par Example
    @code
    acc.open( tcp::v6() );
    acc.set_option( socket_option::reuse_port( true ) );
    acc.bind( endpoint( ipv6_address::any(), 8080 ) );
    acc.listen();
    @endcode
*/
class BOOST_COROSIO_DECL reuse_port : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Set the receive buffer size (SO_RCVBUF).

    @par Example
    @code
    sock.set_option( socket_option::receive_buffer_size( 65536 ) );
    auto opt = sock.get_option<socket_option::receive_buffer_size>();
    int sz = opt.value();
    @endcode
*/
class BOOST_COROSIO_DECL receive_buffer_size : public integer_option
{
public:
    using integer_option::integer_option;
    using integer_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Set the send buffer size (SO_SNDBUF).

    @par Example
    @code
    sock.set_option( socket_option::send_buffer_size( 65536 ) );
    @endcode
*/
class BOOST_COROSIO_DECL send_buffer_size : public integer_option
{
public:
    using integer_option::integer_option;
    using integer_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** The SO_LINGER socket option.

    Controls behavior when closing a socket with unsent data.
    When enabled, `close()` blocks until pending data is sent
    or the timeout expires.

    @par Example
    @code
    sock.set_option( socket_option::linger( true, 5 ) );
    auto opt = sock.get_option<socket_option::linger>();
    if ( opt.enabled() )
        std::cout << "linger timeout: " << opt.timeout() << "s\n";
    @endcode
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

    /// Set whether linger is enabled.
    void enabled(bool v) noexcept;

    /// Return the linger timeout in seconds.
    int timeout() const noexcept;

    /// Set the linger timeout in seconds.
    void timeout(int v) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /** Normalize after `getsockopt`.

        No-op — `struct linger` is always returned at full size.

        @param s The number of bytes actually written by `getsockopt`.
    */
    void resize(std::size_t) noexcept {}
};

/** Enable loopback of outgoing multicast on IPv4 (IP_MULTICAST_LOOP).

    @par Example
    @code
    sock.set_option( socket_option::multicast_loop_v4( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_loop_v4 : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Enable loopback of outgoing multicast on IPv6 (IPV6_MULTICAST_LOOP).

    @par Example
    @code
    sock.set_option( socket_option::multicast_loop_v6( true ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_loop_v6 : public boolean_option
{
public:
    using boolean_option::boolean_option;
    using boolean_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Set the multicast TTL for IPv4 (IP_MULTICAST_TTL).

    @par Example
    @code
    sock.set_option( socket_option::multicast_hops_v4( 4 ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_hops_v4 : public integer_option
{
public:
    using integer_option::integer_option;
    using integer_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Set the multicast hop limit for IPv6 (IPV6_MULTICAST_HOPS).

    @par Example
    @code
    sock.set_option( socket_option::multicast_hops_v6( 4 ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_hops_v6 : public integer_option
{
public:
    using integer_option::integer_option;
    using integer_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Set the outgoing interface for IPv6 multicast (IPV6_MULTICAST_IF).

    @par Example
    @code
    sock.set_option( socket_option::multicast_interface_v6( 1 ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_interface_v6 : public integer_option
{
public:
    using integer_option::integer_option;
    using integer_option::operator=;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;
};

/** Join an IPv4 multicast group (IP_ADD_MEMBERSHIP).

    @par Example
    @code
    sock.set_option( socket_option::join_group_v4(
        ipv4_address( "239.255.0.1" ) ) );
    @endcode
*/
class BOOST_COROSIO_DECL join_group_v4
{
    static constexpr std::size_t max_storage_ = 8;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values.
    join_group_v4() noexcept = default;

    /** Construct with a group and optional interface address.

        @param group The multicast group address to join.
        @param iface The local interface to use (default: any).
    */
    join_group_v4(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Leave an IPv4 multicast group (IP_DROP_MEMBERSHIP).

    @par Example
    @code
    sock.set_option( socket_option::leave_group_v4(
        ipv4_address( "239.255.0.1" ) ) );
    @endcode
*/
class BOOST_COROSIO_DECL leave_group_v4
{
    static constexpr std::size_t max_storage_ = 8;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values.
    leave_group_v4() noexcept = default;

    /** Construct with a group and optional interface address.

        @param group The multicast group address to leave.
        @param iface The local interface (default: any).
    */
    leave_group_v4(
        ipv4_address group, ipv4_address iface = ipv4_address()) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Join an IPv6 multicast group (IPV6_JOIN_GROUP).

    @par Example
    @code
    sock.set_option( socket_option::join_group_v6(
        ipv6_address( "ff02::1" ), 0 ) );
    @endcode
*/
class BOOST_COROSIO_DECL join_group_v6
{
    static constexpr std::size_t max_storage_ = 20;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values.
    join_group_v6() noexcept = default;

    /** Construct with a group and optional interface index.

        @param group The multicast group address to join.
        @param if_index The interface index (0 = kernel chooses).
    */
    join_group_v6(ipv6_address group, unsigned int if_index = 0) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Leave an IPv6 multicast group (IPV6_LEAVE_GROUP).

    @par Example
    @code
    sock.set_option( socket_option::leave_group_v6(
        ipv6_address( "ff02::1" ), 0 ) );
    @endcode
*/
class BOOST_COROSIO_DECL leave_group_v6
{
    static constexpr std::size_t max_storage_ = 20;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values.
    leave_group_v6() noexcept = default;

    /** Construct with a group and optional interface index.

        @param group The multicast group address to leave.
        @param if_index The interface index (0 = kernel chooses).
    */
    leave_group_v6(ipv6_address group, unsigned int if_index = 0) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

/** Set the outgoing interface for IPv4 multicast (IP_MULTICAST_IF).

    Unlike the integer-based `multicast_interface_v6`, this option
    takes an `ipv4_address` identifying the local interface.

    @par Example
    @code
    sock.set_option( socket_option::multicast_interface_v4(
        ipv4_address( "192.168.1.1" ) ) );
    @endcode
*/
class BOOST_COROSIO_DECL multicast_interface_v4
{
    static constexpr std::size_t max_storage_ = 4;
    alignas(4) unsigned char storage_[max_storage_]{};

public:
    /// Construct with default values (INADDR_ANY).
    multicast_interface_v4() noexcept = default;

    /** Construct with an interface address.

        @param iface The local interface address.
    */
    explicit multicast_interface_v4(ipv4_address iface) noexcept;

    /// Return the protocol level.
    static int level() noexcept;

    /// Return the option name.
    static int name() noexcept;

    /// Return a pointer to the underlying storage.
    void* data() noexcept
    {
        return storage_;
    }

    /// Return a pointer to the underlying storage.
    void const* data() const noexcept
    {
        return storage_;
    }

    /// Return the size of the underlying storage.
    std::size_t size() const noexcept;

    /// No-op resize.
    void resize(std::size_t) noexcept {}
};

} // namespace boost::corosio::socket_option

#endif // BOOST_COROSIO_SOCKET_OPTION_HPP
