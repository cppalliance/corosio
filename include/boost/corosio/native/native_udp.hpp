//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/** @file native_udp.hpp

    Inline UDP protocol type using platform-specific constants.
    All methods are `constexpr` or trivially inlined, giving zero
    overhead compared to hand-written socket creation calls.

    This header includes platform socket headers
    (`<sys/socket.h>`, `<netinet/in.h>`, etc.).
    For a version that avoids platform includes, use
    `<boost/corosio/udp.hpp>` (`boost::corosio::udp`).

    Both variants satisfy the same protocol-type interface and work
    interchangeably with `udp_socket::open`.

    @see boost::corosio::udp
*/

#ifndef BOOST_COROSIO_NATIVE_NATIVE_UDP_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_UDP_HPP

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace boost::corosio {

class udp_socket;

} // namespace boost::corosio

namespace boost::corosio {

/** Inline UDP protocol type with platform constants.

    Same shape as @ref boost::corosio::udp but with inline
    `family()`, `type()`, and `protocol()` methods that
    resolve to compile-time constants.

    @see boost::corosio::udp
*/
class native_udp
{
    bool v6_;
    explicit constexpr native_udp(bool v6) noexcept : v6_(v6) {}

public:
    /// Construct an IPv4 UDP protocol.
    static constexpr native_udp v4() noexcept
    {
        return native_udp(false);
    }

    /// Construct an IPv6 UDP protocol.
    static constexpr native_udp v6() noexcept
    {
        return native_udp(true);
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

    /// Return the socket type (SOCK_DGRAM).
    static constexpr int type() noexcept
    {
        return SOCK_DGRAM;
    }

    /// Return the IP protocol (IPPROTO_UDP).
    static constexpr int protocol() noexcept
    {
        return IPPROTO_UDP;
    }

    /// The associated socket type.
    using socket = udp_socket;

    friend constexpr bool operator==(native_udp a, native_udp b) noexcept
    {
        return a.v6_ == b.v6_;
    }

    friend constexpr bool operator!=(native_udp a, native_udp b) noexcept
    {
        return a.v6_ != b.v6_;
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_NATIVE_NATIVE_UDP_HPP
