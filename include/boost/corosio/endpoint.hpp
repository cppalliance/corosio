//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/corosio/ip_address.hpp>

#include <boost/capy/io_result.hpp>

#include <compare>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace boost::corosio {

/** An IP endpoint (address + port) supporting both IPv4 and IPv6.

    This class represents an endpoint for IP communication,
    consisting of an IP address of either family and a port number.
    Endpoints are used to specify connection targets and bind addresses.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.

    @par Example
    @par !example endpoint
*/
class endpoint
{
    ip_address addr_;
    std::uint16_t port_ = 0;

public:
    /** Default constructor.

        Creates an endpoint with the IPv4 any address (0.0.0.0) and port 0.
    */
    endpoint() noexcept = default;

    /** Construct from an IP address and port.

        `ipv4_address` and `ipv6_address` arguments convert
        implicitly, so both families construct directly:
        `endpoint(ipv4_address::loopback(), 80)`.

        @param addr The IP address.
        @param p The port number in host byte order.
    */
    endpoint(ip_address addr, std::uint16_t p) noexcept : addr_(addr), port_(p)
    {
    }

    /** Construct from port only.

        Uses the IPv4 any address (0.0.0.0), which binds to all
        available network interfaces.

        @param p The port number in host byte order.
    */
    explicit endpoint(std::uint16_t p) noexcept : port_(p) {}

    /** Construct from an endpoint's address with a different port.

        Creates a new endpoint using the address from an existing
        endpoint but with a different port number.

        @param ep The endpoint whose address to use.
        @param p The port number in host byte order.
    */
    endpoint(endpoint const& ep, std::uint16_t p) noexcept
        : addr_(ep.addr_)
        , port_(p)
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

        @see make_endpoint for the non-throwing form.
    */
    explicit endpoint(std::string_view s);

    /** Check if this endpoint uses an IPv4 address.

        @return `true` if the endpoint uses IPv4, `false` if IPv6.
    */
    bool is_v4() const noexcept
    {
        return addr_.is_v4();
    }

    /** Check if this endpoint uses an IPv6 address.

        @return `true` if the endpoint uses IPv6, `false` if IPv4.
    */
    bool is_v6() const noexcept
    {
        return addr_.is_v6();
    }

    /** Return the IP address.

        @return The endpoint's address.
    */
    ip_address address() const noexcept
    {
        return addr_;
    }

    /** Return the port number.

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
        return a.port_ == b.port_ && a.addr_ == b.addr_;
    }

    /** Order two endpoints.

        Establishes a strict total ordering consistent with
        @ref operator==: equal endpoints compare equivalent.
        Endpoints are ordered first by address family (IPv4
        before IPv6), then by address value, then by port. This
        makes `endpoint` usable as a key in ordered containers
        such as `std::map` and `std::set`.

        @return The relative order of @p a and @p b.
    */
    friend std::strong_ordering
    operator<=>(endpoint const& a, endpoint const& b) noexcept
    {
        if (auto c = a.addr_ <=> b.addr_; c != 0)
            return c;
        return a.port_ <=> b.port_;
    }
};

/** Endpoint format detection result.

    Used internally by make_endpoint to determine
    the format of an endpoint string.
*/
enum class endpoint_format
{
    ipv4_no_port,   ///< "192.168.1.1"
    ipv4_with_port, ///< "192.168.1.1:8080"
    ipv6_no_port,   ///< "::1" or "1:2:3:4:5:6:7:8"
    ipv6_bracketed  ///< "[::1]" or "[::1]:8080"
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
endpoint_format detect_endpoint_format(std::string_view s) noexcept;

/** Create an endpoint from a string.

    This function parses an endpoint string in one of
    the following formats:

    @li IPv4 without port: `192.168.1.1`
    @li IPv4 with port: `192.168.1.1:8080`
    @li IPv6 without port: `::1` or `2001:db8::1`
    @li IPv6 with port (bracketed): `[::1]:8080`

    @par Example
    @par !example make_endpoint

    @param s The string to parse.
    @return The error code, empty on success, and the parsed
        endpoint — default-constructed on failure.
*/
[[nodiscard]] BOOST_COROSIO_DECL capy::io_result<endpoint>
make_endpoint(std::string_view s) noexcept;

inline endpoint::endpoint(std::string_view s)
{
    auto [ec, ep] = make_endpoint(s);
    if (ec)
        detail::throw_system_error(ec);
    *this = ep;
}

} // namespace boost::corosio

namespace std {

/// Hash support for `boost::corosio::endpoint`.
template<>
struct hash<boost::corosio::endpoint>
{
    /// Return the hash of `ep`.
    std::size_t operator()(boost::corosio::endpoint const& ep) const noexcept
    {
        std::size_t const h1 = hash<boost::corosio::ip_address>()(ep.address());
        std::size_t const h2 = hash<std::uint16_t>()(ep.port());
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

} // namespace std

#endif
