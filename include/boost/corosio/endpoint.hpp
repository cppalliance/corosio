//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_ENDPOINT_HPP
#define BOOST_COROSIO_ENDPOINT_HPP

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/url/ipv4_address.hpp>
#include <boost/url/ipv6_address.hpp>

#include <cstdint>
#include <cstring>

namespace boost {
namespace corosio {

/** An IP endpoint (address + port) supporting both IPv4 and IPv6.

    This class represents an endpoint for IP communication,
    consisting of either an IPv4 or IPv6 address and a port number.
    Endpoints are used to specify connection targets and bind addresses.

    The endpoint holds both address types as separate members (not a union),
    with a discriminator to track which address type is active.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.

    @par Example
    @code
    // IPv4 endpoint
    endpoint ep4(urls::ipv4_address::loopback(), 8080);

    // IPv6 endpoint
    endpoint ep6(urls::ipv6_address::loopback(), 8080);

    // Port only (defaults to IPv4 any address)
    endpoint bind_addr(8080);
    @endcode
*/
class endpoint
{
public:
    /** Default constructor.

        Creates an endpoint with the IPv4 any address (0.0.0.0) and port 0.
    */
    endpoint() noexcept
        : v4_address_(urls::ipv4_address::any())
        , v6_address_{}
        , port_(0)
        , is_v4_(true)
    {
    }

    /** Construct from IPv4 address and port.

        @param addr The IPv4 address.
        @param p The port number in host byte order.
    */
    endpoint(urls::ipv4_address addr, std::uint16_t p) noexcept
        : v4_address_(addr)
        , v6_address_{}
        , port_(p)
        , is_v4_(true)
    {
    }

    /** Construct from IPv6 address and port.

        @param addr The IPv6 address.
        @param p The port number in host byte order.
    */
    endpoint(urls::ipv6_address addr, std::uint16_t p) noexcept
        : v4_address_(urls::ipv4_address::any())
        , v6_address_(addr)
        , port_(p)
        , is_v4_(false)
    {
    }

    /** Construct from port only.

        Uses the IPv4 any address (0.0.0.0), which binds to all
        available network interfaces.

        @param p The port number in host byte order.
    */
    explicit endpoint(std::uint16_t p) noexcept
        : v4_address_(urls::ipv4_address::any())
        , v6_address_{}
        , port_(p)
        , is_v4_(true)
    {
    }

    /** Check if this endpoint uses an IPv4 address.

        @return `true` if the endpoint uses IPv4, `false` if IPv6.
    */
    bool is_v4() const noexcept
    {
        return is_v4_;
    }

    /** Check if this endpoint uses an IPv6 address.

        @return `true` if the endpoint uses IPv6, `false` if IPv4.
    */
    bool is_v6() const noexcept
    {
        return !is_v4_;
    }

    /** Get the IPv4 address.

        @return The IPv4 address. The value is valid even if
        the endpoint is using IPv6 (it will be the default any address).
    */
    urls::ipv4_address v4_address() const noexcept
    {
        return v4_address_;
    }

    /** Get the IPv6 address.

        @return The IPv6 address. The value is valid even if
        the endpoint is using IPv4 (it will be the default any address).
    */
    urls::ipv6_address v6_address() const noexcept
    {
        return v6_address_;
    }

    /** Get the port number.

        @return The port number in host byte order.
    */
    std::uint16_t port() const noexcept
    {
        return port_;
    }

    /** Compare endpoints for equality.

        Two endpoints are equal if they have the same address type,
        the same address value, and the same port.

        @return `true` if both endpoints are equal.
    */
    friend bool operator==(endpoint const& a, endpoint const& b) noexcept
    {
        if (a.is_v4_ != b.is_v4_)
            return false;
        if (a.port_ != b.port_)
            return false;
        if (a.is_v4_)
            return a.v4_address_ == b.v4_address_;
        else
            return a.v6_address_ == b.v6_address_;
    }

    /** Compare endpoints for inequality.

        @return `true` if endpoints differ.
    */
    friend bool operator!=(endpoint const& a, endpoint const& b) noexcept
    {
        return !(a == b);
    }

private:
    urls::ipv4_address v4_address_;
    urls::ipv6_address v6_address_;
    std::uint16_t port_ = 0;
    bool is_v4_ = true;
};

} // namespace corosio
} // namespace boost

#endif
#endif
