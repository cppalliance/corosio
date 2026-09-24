//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_UDP_HPP
#define BOOST_COROSIO_UDP_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/family.hpp>

namespace boost::corosio {

class udp_socket;

/** Encapsulate the UDP protocol for socket creation.

    This class identifies the UDP protocol and its address family
    (IPv4 or IPv6). It is used to parameterize `udp_socket::open()`
    calls with a self-documenting type.

    `family()` returns the portable @ref family the socket will be
    opened with. The `type()` and `protocol()` members return the
    two integers passed to the operating system's `socket()` call;
    their values are platform-defined constants resolved inside
    the library, so this header needs no system socket headers.

    @par Example
    @par !example udp
*/
class BOOST_COROSIO_DECL udp
{
    corosio::family family_;

    explicit constexpr udp(corosio::family f) noexcept : family_(f) {}

public:
    /// Construct an IPv4 UDP protocol.
    static constexpr udp v4() noexcept
    {
        return udp(corosio::family::v4);
    }

    /// Construct an IPv6 UDP protocol.
    static constexpr udp v6() noexcept
    {
        return udp(corosio::family::v6);
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

    /// Return the socket type (SOCK_DGRAM).
    static int type() noexcept;

    /// Return the IP protocol (IPPROTO_UDP).
    static int protocol() noexcept;

    /// The socket type to use with this protocol, @ref udp_socket.
    using socket = udp_socket;

    /// Test for equality.
    friend constexpr bool operator==(udp a, udp b) noexcept
    {
        return a.family_ == b.family_;
    }

    /// Test for inequality.
    friend constexpr bool operator!=(udp a, udp b) noexcept
    {
        return a.family_ != b.family_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_UDP_HPP
