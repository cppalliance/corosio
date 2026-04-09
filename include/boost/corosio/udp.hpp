//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_UDP_HPP
#define BOOST_COROSIO_UDP_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost::corosio {

class udp_socket;

/** Encapsulate the UDP protocol for socket creation.

    This class identifies the UDP protocol and its address family
    (IPv4 or IPv6). It is used to parameterize `udp_socket::open()`
    calls with a self-documenting type.

    The `family()`, `type()`, and `protocol()` members are
    implemented in the compiled library to avoid exposing
    platform socket headers. For an inline variant that includes
    platform headers, use @ref native_udp.

    @par Example
    @code
    udp_socket sock( ioc );
    sock.open( udp::v4() );
    sock.bind( endpoint( ipv4_address::any(), 9000 ) );
    @endcode

    @see native_udp, udp_socket
*/
class BOOST_COROSIO_DECL udp
{
    bool v6_;
    explicit constexpr udp(bool v6) noexcept : v6_(v6) {}

public:
    /// Construct an IPv4 UDP protocol.
    static constexpr udp v4() noexcept
    {
        return udp(false);
    }

    /// Construct an IPv6 UDP protocol.
    static constexpr udp v6() noexcept
    {
        return udp(true);
    }

    /// Return true if this is IPv6.
    constexpr bool is_v6() const noexcept
    {
        return v6_;
    }

    /// Return the internet address family (IPv4 or IPv6).
    int family() const noexcept;

    /// Return the datagram socket type.
    static int type() noexcept;

    /// Return the UDP protocol number.
    static int protocol() noexcept;

    /// The associated socket type.
    using socket = udp_socket;

    /// Test for equality.
    friend constexpr bool operator==(udp a, udp b) noexcept
    {
        return a.v6_ == b.v6_;
    }

    /// Test for inequality.
    friend constexpr bool operator!=(udp a, udp b) noexcept
    {
        return a.v6_ != b.v6_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_UDP_HPP
