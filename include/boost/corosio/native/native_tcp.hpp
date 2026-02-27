//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/** @file native_tcp.hpp

    Inline TCP protocol type using platform-specific constants.
    All methods are `constexpr` or trivially inlined, giving zero
    overhead compared to hand-written socket creation calls.

    This header includes platform socket headers
    (`<sys/socket.h>`, `<netinet/in.h>`, etc.).
    For a version that avoids platform includes, use
    `<boost/corosio/tcp.hpp>` (`boost::corosio::tcp`).

    Both variants satisfy the same protocol-type interface and work
    interchangeably with `tcp_socket::open` / `tcp_acceptor::open`.

    @see boost::corosio::tcp
*/

#ifndef BOOST_COROSIO_NATIVE_NATIVE_TCP_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_TCP_HPP

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace boost::corosio {

class tcp_socket;
class tcp_acceptor;

} // namespace boost::corosio

namespace boost::corosio {

/** Inline TCP protocol type with platform constants.

    Same shape as @ref boost::corosio::tcp but with inline
    `family()`, `type()`, and `protocol()` methods that
    resolve to compile-time constants.

    @see boost::corosio::tcp
*/
class native_tcp
{
    bool v6_;
    explicit constexpr native_tcp(bool v6) noexcept : v6_(v6) {}

public:
    /// Construct an IPv4 TCP protocol.
    static constexpr native_tcp v4() noexcept
    {
        return native_tcp(false);
    }

    /// Construct an IPv6 TCP protocol.
    static constexpr native_tcp v6() noexcept
    {
        return native_tcp(true);
    }

    /// Return true if this is IPv6.
    constexpr bool is_v6() const noexcept
    {
        return v6_;
    }

    /// Return the address family (AF_INET or AF_INET6).
    int family() const noexcept
    {
        return v6_ ? AF_INET6 : AF_INET;
    }

    /// Return the socket type (SOCK_STREAM).
    static constexpr int type() noexcept
    {
        return SOCK_STREAM;
    }

    /// Return the IP protocol (IPPROTO_TCP).
    static constexpr int protocol() noexcept
    {
        return IPPROTO_TCP;
    }

    /// The associated socket type.
    using socket = tcp_socket;

    /// The associated acceptor type.
    using acceptor = tcp_acceptor;

    friend constexpr bool operator==(native_tcp a, native_tcp b) noexcept
    {
        return a.v6_ == b.v6_;
    }

    friend constexpr bool operator!=(native_tcp a, native_tcp b) noexcept
    {
        return a.v6_ != b.v6_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_TCP_HPP
