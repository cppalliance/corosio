//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IPV4_ADDRESS_HPP
#define BOOST_COROSIO_IPV4_ADDRESS_HPP

#include <boost/corosio/detail/config.hpp>

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>

namespace boost::corosio {

/** An IP version 4 style address.

    Objects of this type are used to construct,
    parse, and manipulate IP version 4 addresses.

    @par BNF
    @code
    IPv4address = dec-octet "." dec-octet "." dec-octet "." dec-octet

    dec-octet   = DIGIT                 ; 0-9
                / %x31-39 DIGIT         ; 10-99
                / "1" 2DIGIT            ; 100-199
                / "2" %x30-34 DIGIT     ; 200-249
                / "25" %x30-35          ; 250-255
    @endcode

    @par Specification
    @li <a href="https://en.wikipedia.org/wiki/IPv4">IPv4 (Wikipedia)</a>
    @li <a href="https://datatracker.ietf.org/doc/html/rfc3986#section-3.2.2"
        >3.2.2. Host (rfc3986)</a>

    @see
        @ref parse_ipv4_address,
        @ref ipv6_address.
*/
class BOOST_COROSIO_DECL ipv4_address
{
    std::uint32_t addr_ = 0;

public:
    /** The number of characters in the longest possible IPv4 string.

        The longest IPv4 address string is "255.255.255.255".
    */
    static constexpr std::size_t max_str_len = 15;

    /** The type used to represent an address as an unsigned integer.
    */
    using uint_type = std::uint32_t;

    /** The type used to represent an address as an array of bytes.
    */
    using bytes_type = std::array<unsigned char, 4>;

    /** Default constructor.

        Constructs the unspecified address (0.0.0.0).
    */
    ipv4_address() = default;

    /** Copy constructor.
    */
    ipv4_address(ipv4_address const&) = default;

    /** Copy assignment.

        @return A reference to this object.
    */
    ipv4_address& operator=(ipv4_address const&) = default;

    /** Construct from an unsigned integer.

        This function constructs an address from
        the unsigned integer `u`, where the most
        significant byte forms the first octet
        of the resulting address.

        @param u The integer to construct from.
    */
    explicit
    ipv4_address(uint_type u) noexcept;

    /** Construct from an array of bytes.

        This function constructs an address
        from the array in `bytes`, which is
        interpreted in big-endian.

        @param bytes The value to construct from.
    */
    explicit
    ipv4_address(bytes_type const& bytes) noexcept;

    /** Construct from a string.

        This function constructs an address from
        the string `s`, which must contain a valid
        IPv4 address string or else an exception
        is thrown.

        @note For a non-throwing parse function,
        use @ref parse_ipv4_address.

        @par Exception Safety
        Exceptions thrown on invalid input.

        @throw std::invalid_argument The input failed to parse correctly.

        @param s The string to parse.

        @par Specification
        @li <a href="https://datatracker.ietf.org/doc/html/rfc3986#section-3.2.2"
            >3.2.2. Host (rfc3986)</a>

        @see
            @ref parse_ipv4_address.
    */
    explicit
    ipv4_address(std::string_view s);

    /** Return the address as bytes, in network byte order.

        @return The address as an array of bytes.
    */
    bytes_type
    to_bytes() const noexcept;

    /** Return the address as an unsigned integer.

        @return The address as an unsigned integer.
    */
    uint_type
    to_uint() const noexcept;

    /** Return the address as a string in dotted decimal format.

        @par Example
        @code
        assert( ipv4_address(0x01020304).to_string() == "1.2.3.4" );
        @endcode

        @return The address as a string.
    */
    std::string
    to_string() const;

    /** Write a dotted decimal string representing the address to a buffer.

        The resulting buffer is not null-terminated.

        @throw std::length_error `dest_size < ipv4_address::max_str_len`

        @return The formatted string view.

        @param dest The buffer in which to write,
        which must have at least `dest_size` space.

        @param dest_size The size of the output buffer.
    */
    std::string_view
    to_buffer(char* dest, std::size_t dest_size) const;

    /** Return true if the address is a loopback address.

        @return `true` if the address is a loopback address.
    */
    bool
    is_loopback() const noexcept;

    /** Return true if the address is unspecified.

        @return `true` if the address is unspecified.
    */
    bool
    is_unspecified() const noexcept;

    /** Return true if the address is a multicast address.

        @return `true` if the address is a multicast address.
    */
    bool
    is_multicast() const noexcept;

    /** Return true if two addresses are equal.

        @return `true` if the addresses are equal, otherwise `false`.
    */
    friend
    bool
    operator==(ipv4_address const& a1, ipv4_address const& a2) noexcept
    {
        return a1.addr_ == a2.addr_;
    }

    /** Return true if two addresses are not equal.

        @return `true` if the addresses are not equal, otherwise `false`.
    */
    friend
    bool
    operator!=(ipv4_address const& a1, ipv4_address const& a2) noexcept
    {
        return a1.addr_ != a2.addr_;
    }

    /** Return an address object that represents any address.

        @return The any address (0.0.0.0).
    */
    static
    ipv4_address
    any() noexcept
    {
        return ipv4_address();
    }

    /** Return an address object that represents the loopback address.

        @return The loopback address (127.0.0.1).
    */
    static
    ipv4_address
    loopback() noexcept
    {
        return ipv4_address(0x7F000001);
    }

    /** Return an address object that represents the broadcast address.

        @return The broadcast address (255.255.255.255).
    */
    static
    ipv4_address
    broadcast() noexcept
    {
        return ipv4_address(0xFFFFFFFF);
    }

    /** Format the address to an output stream.

        IPv4 addresses written to output streams
        are written in their dotted decimal format.

        @param os The output stream.
        @param addr The address to format.
        @return The output stream.
    */
    friend
    std::ostream&
    operator<<(std::ostream& os, ipv4_address const& addr);

private:
    friend class ipv6_address;

    std::size_t
    print_impl(char* dest) const noexcept;
};

//------------------------------------------------

/** Return an IPv4 address from an IP address string in dotted decimal form.

    @param s The string to parse.
    @param addr The address to store the result.
    @return An error code (empty on success).
*/
[[nodiscard]] BOOST_COROSIO_DECL
std::error_code
parse_ipv4_address(
    std::string_view s,
    ipv4_address& addr) noexcept;

} // namespace boost::corosio

#endif
