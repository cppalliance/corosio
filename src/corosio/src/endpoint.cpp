//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/endpoint.hpp>

#include <charconv>

namespace boost::corosio {

endpoint_format
detect_endpoint_format(std::string_view s) noexcept
{
    if (s.empty())
        return endpoint_format::ipv4_no_port;

    // Bracketed IPv6
    if (s[0] == '[')
        return endpoint_format::ipv6_bracketed;

    // Count colons
    std::size_t colon_count = 0;
    for (char c : s)
    {
        if (c == ':')
            ++colon_count;
    }

    if (colon_count == 0)
        return endpoint_format::ipv4_no_port;
    if (colon_count == 1)
        return endpoint_format::ipv4_with_port;
    return endpoint_format::ipv6_no_port;
}

namespace {

// Parse port number from string
// Returns true on success
bool
parse_port(std::string_view s, std::uint16_t& port) noexcept
{
    if (s.empty())
        return false;

    // No leading zeros allowed (except "0" itself)
    if (s.size() > 1 && s[0] == '0')
        return false;

    unsigned long val = 0;
    auto [ptr, ec]    = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return false;
    if (val > 65535)
        return false;

    port = static_cast<std::uint16_t>(val);
    return true;
}

} // namespace

std::error_code
parse_endpoint(std::string_view s, endpoint& ep) noexcept
{
    if (s.empty())
        return std::make_error_code(std::errc::invalid_argument);

    auto fmt = detect_endpoint_format(s);

    switch (fmt)
    {
    case endpoint_format::ipv4_no_port:
    {
        ipv4_address addr;
        auto ec = parse_ipv4_address(s, addr);
        if (ec)
            return ec;
        ep = endpoint(addr, 0);
        return {};
    }

    case endpoint_format::ipv4_with_port:
    {
        // Find the colon separating address and port
        auto colon_pos = s.rfind(':');
        if (colon_pos == std::string_view::npos)
            return std::make_error_code(std::errc::invalid_argument);

        auto addr_str = s.substr(0, colon_pos);
        auto port_str = s.substr(colon_pos + 1);

        ipv4_address addr;
        auto ec = parse_ipv4_address(addr_str, addr);
        if (ec)
            return ec;

        std::uint16_t port;
        if (!parse_port(port_str, port))
            return std::make_error_code(std::errc::invalid_argument);

        ep = endpoint(addr, port);
        return {};
    }

    case endpoint_format::ipv6_no_port:
    {
        ipv6_address addr;
        auto ec = parse_ipv6_address(s, addr);
        if (ec)
            return ec;
        ep = endpoint(addr, 0);
        return {};
    }

    case endpoint_format::ipv6_bracketed:
    {
        // Must start with '[' and contain ']'
        if (s.size() < 2 || s[0] != '[')
            return std::make_error_code(std::errc::invalid_argument);

        auto close_bracket = s.find(']');
        if (close_bracket == std::string_view::npos)
            return std::make_error_code(std::errc::invalid_argument);

        auto addr_str = s.substr(1, close_bracket - 1);

        ipv6_address addr;
        auto ec = parse_ipv6_address(addr_str, addr);
        if (ec)
            return ec;

        std::uint16_t port = 0;
        if (close_bracket + 1 < s.size())
        {
            // There's something after ']'
            if (s[close_bracket + 1] != ':')
                return std::make_error_code(std::errc::invalid_argument);

            auto port_str = s.substr(close_bracket + 2);
            if (!parse_port(port_str, port))
                return std::make_error_code(std::errc::invalid_argument);
        }

        ep = endpoint(addr, port);
        return {};
    }

    default:
        return std::make_error_code(std::errc::invalid_argument);
    }
}

} // namespace boost::corosio
