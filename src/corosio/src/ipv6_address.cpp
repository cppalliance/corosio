//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/ipv4_address.hpp>

#include <cstring>
#include <ostream>
#include <stdexcept>

namespace boost::corosio {

ipv6_address::ipv6_address(bytes_type const& bytes) noexcept
{
    std::memcpy(addr_.data(), bytes.data(), 16);
}

ipv6_address::ipv6_address(ipv4_address const& addr) noexcept
{
    auto const v = addr.to_bytes();
    addr_        = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, v[0], v[1], v[2], v[3]}};
}

ipv6_address::ipv6_address(std::string_view s)
{
    auto ec = parse_ipv6_address(s, *this);
    if (ec)
        throw std::invalid_argument("invalid IPv6 address");
}

std::string
ipv6_address::to_string() const
{
    char buf[max_str_len];
    auto n = print_impl(buf);
    return std::string(buf, n);
}

std::string_view
ipv6_address::to_buffer(char* dest, std::size_t dest_size) const
{
    if (dest_size < max_str_len)
        throw std::length_error("buffer too small for IPv6 address");
    auto n = print_impl(dest);
    return std::string_view(dest, n);
}

bool
ipv6_address::is_unspecified() const noexcept
{
    return *this == ipv6_address();
}

bool
ipv6_address::is_loopback() const noexcept
{
    return *this == loopback();
}

bool
ipv6_address::is_v4_mapped() const noexcept
{
    return addr_[0] == 0 && addr_[1] == 0 && addr_[2] == 0 && addr_[3] == 0 &&
        addr_[4] == 0 && addr_[5] == 0 && addr_[6] == 0 && addr_[7] == 0 &&
        addr_[8] == 0 && addr_[9] == 0 && addr_[10] == 0xff &&
        addr_[11] == 0xff;
}

ipv6_address
ipv6_address::loopback() noexcept
{
    ipv6_address a;
    a.addr_[15] = 1;
    return a;
}

std::ostream&
operator<<(std::ostream& os, ipv6_address const& addr)
{
    char buf[ipv6_address::max_str_len];
    os << addr.to_buffer(buf, sizeof(buf));
    return os;
}

std::size_t
ipv6_address::print_impl(char* dest) const noexcept
{
    auto const count_zeroes = [](unsigned char const* first,
                                 unsigned char const* const last) {
        std::size_t n = 0;
        while (first != last)
        {
            if (first[0] != 0 || first[1] != 0)
                break;
            n += 2;
            first += 2;
        }
        return n;
    };

    auto const print_hex = [](char* dest, unsigned short v) {
        char const* const dig = "0123456789abcdef";
        if (v >= 0x1000)
        {
            *dest++ = dig[v >> 12];
            v &= 0x0fff;
            *dest++ = dig[v >> 8];
            v &= 0x0ff;
            *dest++ = dig[v >> 4];
            v &= 0x0f;
            *dest++ = dig[v];
        }
        else if (v >= 0x100)
        {
            *dest++ = dig[v >> 8];
            v &= 0x0ff;
            *dest++ = dig[v >> 4];
            v &= 0x0f;
            *dest++ = dig[v];
        }
        else if (v >= 0x10)
        {
            *dest++ = dig[v >> 4];
            v &= 0x0f;
            *dest++ = dig[v];
        }
        else
        {
            *dest++ = dig[v];
        }
        return dest;
    };

    auto const dest0 = dest;
    // find longest run of zeroes
    std::size_t best_len = 0;
    int best_pos         = -1;
    auto it              = addr_.data();
    auto const v4        = is_v4_mapped();
    auto const end       = v4 ? (it + addr_.size() - 4) : it + addr_.size();

    while (it != end)
    {
        auto n = count_zeroes(it, end);
        if (n == 0)
        {
            it += 2;
            continue;
        }
        if (n > best_len)
        {
            best_pos = static_cast<int>(it - addr_.data());
            best_len = n;
        }
        it += n;
    }

    it = addr_.data();
    if (best_pos != 0)
    {
        unsigned short v = static_cast<unsigned short>(it[0] * 256U + it[1]);
        dest             = print_hex(dest, v);
        it += 2;
    }
    else
    {
        *dest++ = ':';
        it += best_len;
        if (it == end)
            *dest++ = ':';
    }

    while (it != end)
    {
        *dest++ = ':';
        if (it - addr_.data() == best_pos)
        {
            it += best_len;
            if (it == end)
                *dest++ = ':';
            continue;
        }
        unsigned short v = static_cast<unsigned short>(it[0] * 256U + it[1]);
        dest             = print_hex(dest, v);
        it += 2;
    }

    if (v4)
    {
        ipv4_address::bytes_type bytes;
        bytes[0] = it[0];
        bytes[1] = it[1];
        bytes[2] = it[2];
        bytes[3] = it[3];
        ipv4_address a(bytes);
        *dest++ = ':';
        char buf[ipv4_address::max_str_len];
        auto sv = a.to_buffer(buf, sizeof(buf));
        std::memcpy(dest, sv.data(), sv.size());
        dest += sv.size();
    }

    return static_cast<std::size_t>(dest - dest0);
}

namespace {

// Convert hex character to value (0-15), or -1 if not hex
inline int
hexdig_value(char c) noexcept
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Parse h16 (1-4 hex digits) returning 16-bit value
// Returns true on success, advances `it`
bool
parse_h16(
    char const*& it,
    char const* end,
    unsigned char& hi,
    unsigned char& lo) noexcept
{
    if (it == end)
        return false;

    int d = hexdig_value(*it);
    if (d < 0)
        return false;

    unsigned v = static_cast<unsigned>(d);
    ++it;

    for (int i = 0; i < 3 && it != end; ++i)
    {
        d = hexdig_value(*it);
        if (d < 0)
            break;
        v = (v << 4) | static_cast<unsigned>(d);
        ++it;
    }

    hi = static_cast<unsigned char>((v >> 8) & 0xff);
    lo = static_cast<unsigned char>(v & 0xff);
    return true;
}

// Check if a hex word could be 0..255 if interpreted as decimal
bool
maybe_octet(unsigned char const* p) noexcept
{
    unsigned short word = static_cast<unsigned short>(p[0]) * 256 +
        static_cast<unsigned short>(p[1]);
    if (word > 0x255)
        return false;
    if (((word >> 4) & 0xf) > 9)
        return false;
    if ((word & 0xf) > 9)
        return false;
    return true;
}

} // namespace

std::error_code
parse_ipv6_address(std::string_view s, ipv6_address& addr) noexcept
{
    auto it        = s.data();
    auto const end = it + s.size();

    int n     = 8;     // words needed
    int b     = -1;    // value of n when '::' seen
    bool c    = false; // need colon
    auto prev = it;
    ipv6_address::bytes_type bytes{};
    unsigned char hi, lo;

    for (;;)
    {
        if (it == end)
        {
            if (b != -1)
            {
                // end in "::"
                break;
            }
            // not enough words
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (*it == ':')
        {
            ++it;
            if (it == end)
            {
                // expected ':'
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (*it == ':')
            {
                if (b == -1)
                {
                    // first "::"
                    ++it;
                    --n;
                    b = n;
                    if (n == 0)
                        break;
                    c = false;
                    continue;
                }
                // extra "::" found
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (c)
            {
                prev = it;
                if (!parse_h16(it, end, hi, lo))
                    return std::make_error_code(std::errc::invalid_argument);
                bytes[2 * (8 - n) + 0] = hi;
                bytes[2 * (8 - n) + 1] = lo;
                --n;
                if (n == 0)
                    break;
                continue;
            }
            // expected h16
            return std::make_error_code(std::errc::invalid_argument);
        }

        if (*it == '.')
        {
            if (b == -1 && n > 1)
            {
                // not enough h16
                return std::make_error_code(std::errc::invalid_argument);
            }
            if (!maybe_octet(&bytes[std::size_t(2) * std::size_t(7 - n)]))
            {
                // invalid octet
                return std::make_error_code(std::errc::invalid_argument);
            }
            // rewind the h16 and parse it as IPv4
            it = prev;
            ipv4_address v4;
            auto ec = parse_ipv4_address(
                std::string_view(it, static_cast<std::size_t>(end - it)), v4);
            if (ec)
                return ec;
            // Must consume exactly the IPv4 address portion
            // Re-parse to find where it ends
            auto v4_it = it;
            while (v4_it != end &&
                   (*v4_it == '.' || (*v4_it >= '0' && *v4_it <= '9')))
                ++v4_it;
            // Verify it parsed correctly by re-parsing the exact substring
            ipv4_address v4_check;
            ec = parse_ipv4_address(
                std::string_view(it, static_cast<std::size_t>(v4_it - it)),
                v4_check);
            if (ec)
                return ec;
            it                     = v4_it;
            auto const b4          = v4_check.to_bytes();
            bytes[2 * (7 - n) + 0] = b4[0];
            bytes[2 * (7 - n) + 1] = b4[1];
            bytes[2 * (7 - n) + 2] = b4[2];
            bytes[2 * (7 - n) + 3] = b4[3];
            --n;
            break;
        }

        auto d = hexdig_value(*it);
        if (b != -1 && d < 0)
        {
            // ends in "::"
            break;
        }

        if (!c)
        {
            prev = it;
            if (!parse_h16(it, end, hi, lo))
                return std::make_error_code(std::errc::invalid_argument);
            bytes[2 * (8 - n) + 0] = hi;
            bytes[2 * (8 - n) + 1] = lo;
            --n;
            if (n == 0)
                break;
            c = true;
            continue;
        }

        // ':' divides a word
        return std::make_error_code(std::errc::invalid_argument);
    }

    // Must have consumed entire string
    if (it != end)
        return std::make_error_code(std::errc::invalid_argument);

    if (b == -1)
    {
        addr = ipv6_address{bytes};
        return {};
    }

    if (b == n)
    {
        // "::" last
        auto const i = 2 * (7 - n);
        std::memset(&bytes[i], 0, 16 - i);
    }
    else if (b == 7)
    {
        // "::" first
        auto const i = 2 * (b - n);
        std::memmove(&bytes[16 - i], &bytes[2], i);
        std::memset(&bytes[0], 0, 16 - i);
    }
    else
    {
        // "::" in middle
        auto const i0 = 2 * (7 - b);
        auto const i1 = 2 * (b - n);
        std::memmove(&bytes[16 - i1], &bytes[i0 + 2], i1);
        std::memset(&bytes[i0], 0, 16 - (i0 + i1));
    }

    addr = ipv6_address{bytes};
    return {};
}

} // namespace boost::corosio
