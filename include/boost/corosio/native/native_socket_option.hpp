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

#include <cstddef>

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

/// Set the receive buffer size (SO_RCVBUF).
using receive_buffer_size = integer<SOL_SOCKET, SO_RCVBUF>;

/// Set the send buffer size (SO_SNDBUF).
using send_buffer_size = integer<SOL_SOCKET, SO_SNDBUF>;

#ifdef SO_REUSEPORT
/// Allow multiple sockets to bind to the same port (SO_REUSEPORT).
using reuse_port = boolean<SOL_SOCKET, SO_REUSEPORT>;
#endif

} // namespace boost::corosio::native_socket_option

#endif // BOOST_COROSIO_NATIVE_NATIVE_SOCKET_OPTION_HPP
