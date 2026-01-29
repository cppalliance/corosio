//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/ipv4_address.hpp>

#include <ostream>
#include <stdexcept>

namespace boost::corosio {

ipv4_address::ipv4_address(uint_type u) noexcept
    : addr_(u)
{
}

ipv4_address::ipv4_address(bytes_type const& bytes) noexcept
{
    addr_ =
        (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) <<  8) |
        (static_cast<std::uint32_t>(bytes[3]));
}

ipv4_address::ipv4_address(std::string_view s)
{
    auto ec = parse_ipv4_address(s, *this);
    if (ec)
        throw std::invalid_argument("invalid IPv4 address");
}

auto
ipv4_address::to_bytes() const noexcept -> bytes_type
{
    bytes_type bytes;
    bytes[0] = static_cast<unsigned char>((addr_ >> 24) & 0xff);
    bytes[1] = static_cast<unsigned char>((addr_ >> 16) & 0xff);
    bytes[2] = static_cast<unsigned char>((addr_ >>  8) & 0xff);
    bytes[3] = static_cast<unsigned char>( addr_        & 0xff);
    return bytes;
}

auto
ipv4_address::to_uint() const noexcept -> uint_type
{
    return addr_;
}

std::string
ipv4_address::to_string() const
{
    char buf[max_str_len];
    auto n = print_impl(buf);
    return std::string(buf, n);
}

std::string_view
ipv4_address::to_buffer(char* dest, std::size_t dest_size) const
{
    if (dest_size < max_str_len)
        throw std::length_error("buffer too small for IPv4 address");
    auto n = print_impl(dest);
    return std::string_view(dest, n);
}

bool
ipv4_address::is_loopback() const noexcept
{
    return (addr_ & 0xFF000000) == 0x7F000000;
}

bool
ipv4_address::is_unspecified() const noexcept
{
    return addr_ == 0;
}

bool
ipv4_address::is_multicast() const noexcept
{
    return (addr_ & 0xF0000000) == 0xE0000000;
}

std::ostream&
operator<<(std::ostream& os, ipv4_address const& addr)
{
    char buf[ipv4_address::max_str_len];
    os << addr.to_buffer(buf, sizeof(buf));
    return os;
}

std::size_t
ipv4_address::print_impl(char* dest) const noexcept
{
    auto const start = dest;
    auto const write = [](char*& dest, unsigned char v)
    {
        if (v >= 100)
        {
            *dest++ = '0' + v / 100;
            v %= 100;
            *dest++ = '0' + v / 10;
            v %= 10;
        }
        else if (v >= 10)
        {
            *dest++ = '0' + v / 10;
            v %= 10;
        }
        *dest++ = '0' + v;
    };
    write(dest, static_cast<unsigned char>((addr_ >> 24) & 0xff));
    *dest++ = '.';
    write(dest, static_cast<unsigned char>((addr_ >> 16) & 0xff));
    *dest++ = '.';
    write(dest, static_cast<unsigned char>((addr_ >>  8) & 0xff));
    *dest++ = '.';
    write(dest, static_cast<unsigned char>( addr_        & 0xff));
    return static_cast<std::size_t>(dest - start);
}

//------------------------------------------------

namespace {

// Parse a decimal octet (0-255), no leading zeros except "0"
// Returns true on success, advances `it`
bool
parse_dec_octet(
    char const*& it,
    char const* end,
    unsigned char& octet) noexcept
{
    if (it == end)
        return false;

    char c = *it;
    if (c < '0' || c > '9')
        return false;

    unsigned v = static_cast<unsigned>(c - '0');
    ++it;

    if (v == 0)
    {
        // "0" is valid, but "00", "01", etc. are not
        if (it != end && *it >= '0' && *it <= '9')
            return false;
        octet = 0;
        return true;
    }

    // First digit was 1-9, can have more
    if (it != end && *it >= '0' && *it <= '9')
    {
        v = v * 10 + static_cast<unsigned>(*it - '0');
        ++it;

        if (it != end && *it >= '0' && *it <= '9')
        {
            v = v * 10 + static_cast<unsigned>(*it - '0');
            ++it;

            // Can't have more than 3 digits
            if (it != end && *it >= '0' && *it <= '9')
                return false;
        }
    }

    if (v > 255)
        return false;

    octet = static_cast<unsigned char>(v);
    return true;
}

} // namespace

std::error_code
parse_ipv4_address(
    std::string_view s,
    ipv4_address& addr) noexcept
{
    auto it = s.data();
    auto const end = it + s.size();

    unsigned char octets[4];

    // Parse first octet
    if (!parse_dec_octet(it, end, octets[0]))
        return std::make_error_code(std::errc::invalid_argument);

    // Parse remaining octets
    for (int i = 1; i < 4; ++i)
    {
        if (it == end || *it != '.')
            return std::make_error_code(std::errc::invalid_argument);
        ++it; // skip '.'

        if (!parse_dec_octet(it, end, octets[i]))
            return std::make_error_code(std::errc::invalid_argument);
    }

    // Must have consumed entire string
    if (it != end)
        return std::make_error_code(std::errc::invalid_argument);

    addr = ipv4_address(ipv4_address::bytes_type{{
        octets[0], octets[1], octets[2], octets[3]}});

    return {};
}

} // namespace boost::corosio
