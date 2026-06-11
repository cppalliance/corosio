//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_HPP
#define BOOST_COROSIO_TCP_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

class tcp_socket;
class tcp_acceptor;

/** Encapsulate the TCP protocol for socket creation.

    This class identifies the TCP protocol and its address family
    (IPv4 or IPv6). It is used to parameterize socket and acceptor
    `open()` calls with a self-documenting type.

    The `family()`, `type()`, and `protocol()` members return the
    three integers passed to the operating system's `socket()`
    call. Their values are platform-defined constants taken from
    the system socket headers. For an inline variant that includes
    those headers, use @ref native_tcp.

    @par Example
    @code
    tcp_acceptor acc( ioc );
    acc.open( tcp::v6() );  // IPv6 socket
    acc.set_option( socket_option::reuse_address( true ) );
    acc.bind( endpoint( ipv6_address::any(), 8080 ) );
    acc.listen();
    @endcode

    @see native_tcp, tcp_socket, tcp_acceptor
*/
class BOOST_COROSIO_DECL tcp
{
    bool v6_;
    explicit constexpr tcp(bool v6) noexcept : v6_(v6) {}

public:
    /// Construct an IPv4 TCP protocol.
    static constexpr tcp v4() noexcept
    {
        return tcp(false);
    }

    /// Construct an IPv6 TCP protocol.
    static constexpr tcp v6() noexcept
    {
        return tcp(true);
    }

    /// Return true if this is IPv6.
    constexpr bool is_v6() const noexcept
    {
        return v6_;
    }

    /// Return the address family (AF_INET or AF_INET6).
    int family() const noexcept;

    /// Return the socket type (SOCK_STREAM).
    static int type() noexcept;

    /// Return the IP protocol (IPPROTO_TCP).
    static int protocol() noexcept;

    /// The socket type to use with this protocol, @ref tcp_socket.
    using socket = tcp_socket;

    /// The acceptor type to use with this protocol, @ref tcp_acceptor.
    using acceptor = tcp_acceptor;

    /// Test for equality.
    friend constexpr bool operator==(tcp a, tcp b) noexcept
    {
        return a.v6_ == b.v6_;
    }

    /// Test for inequality.
    friend constexpr bool operator!=(tcp a, tcp b) noexcept
    {
        return a.v6_ != b.v6_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_TCP_HPP
