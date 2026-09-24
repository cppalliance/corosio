//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TCP_HPP
#define BOOST_COROSIO_TCP_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/family.hpp>

namespace boost::corosio {

class tcp_socket;
class tcp_acceptor;

/** Encapsulate the TCP protocol for socket creation.

    This class identifies the TCP protocol and its address family
    (IPv4 or IPv6). It is used to parameterize socket and acceptor
    `open()` calls with a self-documenting type.

    `family()` returns the portable @ref family the socket will be
    opened with. The `type()` and `protocol()` members return the
    two integers passed to the operating system's `socket()` call;
    their values are platform-defined constants resolved inside
    the library, so this header needs no system socket headers.

    @par Example
    @par !example tcp
*/
class BOOST_COROSIO_DECL tcp
{
    corosio::family family_;

    explicit constexpr tcp(corosio::family f) noexcept : family_(f) {}

public:
    /// Construct an IPv4 TCP protocol.
    static constexpr tcp v4() noexcept
    {
        return tcp(corosio::family::v4);
    }

    /// Construct an IPv6 TCP protocol.
    static constexpr tcp v6() noexcept
    {
        return tcp(corosio::family::v6);
    }

    /// Return true if this is IPv6.
    constexpr bool is_v6() const noexcept
    {
        return family_ == corosio::family::v6;
    }

    /// Return the address family.
    constexpr corosio::family family() const noexcept
    {
        return family_;
    }

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
        return a.family_ == b.family_;
    }

    /// Test for inequality.
    friend constexpr bool operator!=(tcp a, tcp b) noexcept
    {
        return a.family_ != b.family_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_TCP_HPP
