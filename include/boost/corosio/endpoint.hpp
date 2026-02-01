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

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include <cstdint>
#include <string_view>
#include <system_error>

namespace boost::corosio {

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
    endpoint ep4(ipv4_address::loopback(), 8080);

    // IPv6 endpoint
    endpoint ep6(ipv6_address::loopback(), 8080);

    // Port only (defaults to IPv4 any address)
    endpoint bind_addr(8080);

    // Parse from string
    endpoint ep;
    if (auto ec = parse_endpoint("192.168.1.1:8080", ep); !ec) {
        // use ep
    }
    @endcode
*/
class endpoint
{
    ipv4_address v4_address_;
    ipv6_address v6_address_;
    std::uint16_t port_ = 0;
    bool is_v4_ = true;

public:
    /** Default constructor.

        Creates an endpoint with the IPv4 any address (0.0.0.0) and port 0.
    */
    endpoint() noexcept
        : v4_address_(ipv4_address::any())
        , v6_address_{}
        , port_(0)
        , is_v4_(true)
    {
    }

    /** Construct from IPv4 address and port.

        @param addr The IPv4 address.
        @param p The port number in host byte order.
    */
    endpoint(ipv4_address addr, std::uint16_t p) noexcept
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
    endpoint(ipv6_address addr, std::uint16_t p) noexcept
        : v4_address_(ipv4_address::any())
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
        : v4_address_(ipv4_address::any())
        , v6_address_{}
        , port_(p)
        , is_v4_(true)
    {
    }

    /** Construct from an endpoint's address with a different port.

        Creates a new endpoint using the address from an existing
        endpoint but with a different port number.

        @param ep The endpoint whose address to use.
        @param p The port number in host byte order.
    */
    endpoint(endpoint const& ep, std::uint16_t p) noexcept
        : v4_address_(ep.v4_address_)
        , v6_address_(ep.v6_address_)
        , port_(p)
        , is_v4_(ep.is_v4_)
    {
    }

    /** Construct from a string.

        Parses an endpoint string in one of the following formats:
        @li IPv4 without port: `192.168.1.1`
        @li IPv4 with port: `192.168.1.1:8080`
        @li IPv6 without port: `::1` or `2001:db8::1`
        @li IPv6 with port (bracketed): `[::1]:8080`

        @param s The string to parse.

        @throws std::system_error on parse failure.
    */
    explicit endpoint(std::string_view s);

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
    ipv4_address v4_address() const noexcept
    {
        return v4_address_;
    }

    /** Get the IPv6 address.

        @return The IPv6 address. The value is valid even if
        the endpoint is using IPv4 (it will be the default any address).
    */
    ipv6_address v6_address() const noexcept
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
};

//------------------------------------------------

/** Endpoint format detection result.

    Used internally by parse_endpoint to determine
    the format of an endpoint string.
*/
enum class endpoint_format
{
    ipv4_no_port,      ///< "192.168.1.1"
    ipv4_with_port,    ///< "192.168.1.1:8080"
    ipv6_no_port,      ///< "::1" or "1:2:3:4:5:6:7:8"
    ipv6_bracketed     ///< "[::1]" or "[::1]:8080"
};

/** Detect the format of an endpoint string.

    This helper function determines the endpoint format
    based on simple rules:
    1. Starts with `[` -> `ipv6_bracketed`
    2. Else count `:` characters:
       - 0 colons -> `ipv4_no_port`
       - 1 colon -> `ipv4_with_port`
       - 2+ colons -> `ipv6_no_port`

    @param s The string to analyze.
    @return The detected endpoint format.
*/
BOOST_COROSIO_DECL
endpoint_format
detect_endpoint_format(std::string_view s) noexcept;

/** Parse an endpoint from a string.

    This function parses an endpoint string in one of
    the following formats:

    @li IPv4 without port: `192.168.1.1`
    @li IPv4 with port: `192.168.1.1:8080`
    @li IPv6 without port: `::1` or `2001:db8::1`
    @li IPv6 with port (bracketed): `[::1]:8080`

    @par Example
    @code
    endpoint ep;
    if (auto ec = parse_endpoint("192.168.1.1:8080", ep); !ec) {
        // ep.is_v4() == true
        // ep.port() == 8080
    }

    if (auto ec = parse_endpoint("[::1]:443", ep); !ec) {
        // ep.is_v6() == true
        // ep.port() == 443
    }
    @endcode

    @param s The string to parse.
    @param ep The endpoint to store the result.
    @return An error code (empty on success).
*/
[[nodiscard]] BOOST_COROSIO_DECL
std::error_code
parse_endpoint(
    std::string_view s,
    endpoint& ep) noexcept;

inline
endpoint::endpoint(std::string_view s)
{
    auto ec = parse_endpoint(s, *this);
    if (ec)
        detail::throw_system_error(ec);
}

} // namespace boost::corosio

#endif
