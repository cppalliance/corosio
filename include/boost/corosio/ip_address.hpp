//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IP_ADDRESS_HPP
#define BOOST_COROSIO_IP_ADDRESS_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/family.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

#include <boost/capy/io_result.hpp>

#include <compare>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>

namespace boost::corosio {

/** A version-independent IP address.

    This class holds either an IPv4 or an IPv6 address. Code that works with
    both families carries one value instead of branching between @ref
    ipv4_address and @ref ipv6_address. Family-generic queries such as @ref
    is_loopback dispatch to the held address, and @ref to_v4 / @ref to_v6
    recover the family-specific form.

    A v4-mapped IPv6 address (`::ffff:a.b.c.d`) is an IPv6-family
    value: it does not compare equal to the IPv4 address it maps.
    To compare across the mapping, normalize both sides with
    @ref to_v4 first.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.

    @par Example
    @code
    ip_address addr("2001:db8::1");
    if (addr.is_loopback())
    {
        // family-generic query, no branching
    }
    @endcode

    @see
        @ref ipv4_address,
        @ref ipv6_address,
        @ref make_ip_address.
*/
class BOOST_COROSIO_DECL ip_address
{
    ipv4_address v4_;
    ipv6_address v6_;
    corosio::family family_ = corosio::family::v4;

public:
    /** The number of characters in the longest possible address string.
    */
    static constexpr std::size_t max_str_len = ipv6_address::max_str_len;

    /** Default constructor.

        Constructs the IPv4 unspecified address (0.0.0.0).
    */
    ip_address() = default;

    /** Copy constructor.
    */
    ip_address(ip_address const&) = default;

    /** Copy assignment.

        @return A reference to this object.
    */
    ip_address& operator=(ip_address const&) = default;

    /** Construct from an IPv4 address.

        @param addr The address to hold.
    */
    ip_address(ipv4_address const& addr) noexcept : v4_(addr) {}

    /** Construct from an IPv6 address.

        @param addr The address to hold.
    */
    ip_address(ipv6_address const& addr) noexcept
        : v6_(addr)
        , family_(corosio::family::v6)
    {
    }

    /** Construct from a string.

        This function constructs an address from the string `s`,
        which must contain a valid IPv4 or IPv6 address string
        or else an exception is thrown.

        @par Exception Safety
        Strong guarantee.

        @throws std::system_error `errc::invalid_argument` if the input
        failed to parse correctly.

        @note For a non-throwing parse function,
        use @ref make_ip_address.

        @param s The string to parse.

        @see
            @ref make_ip_address.
    */
    explicit ip_address(std::string_view s);

    /** Return the address family.

        The portable spelling of the family; @ref is_v4 and
        @ref is_v6 are sugar over it.

        @return The family of the held address.
    */
    corosio::family family() const noexcept
    {
        return family_;
    }

    /** Check if the held address is IPv4.

        @return `true` if the address is IPv4, `false` if IPv6.
    */
    bool is_v4() const noexcept
    {
        return family_ == corosio::family::v4;
    }

    /** Check if the held address is IPv6.

        @return `true` if the address is IPv6, `false` if IPv4.
    */
    bool is_v6() const noexcept
    {
        return family_ == corosio::family::v6;
    }

    /** Check if the address is a loopback address.

        @return `true` if the held address is a loopback
        address of its family.
    */
    bool is_loopback() const noexcept
    {
        return is_v4() ? v4_.is_loopback() : v6_.is_loopback();
    }

    /** Check if the address is unspecified.

        @return `true` if the held address is the unspecified
        address of its family.
    */
    bool is_unspecified() const noexcept
    {
        return is_v4() ? v4_.is_unspecified() : v6_.is_unspecified();
    }

    /** Check if the address is a multicast address.

        @return `true` if the held address is a multicast
        address of its family.
    */
    bool is_multicast() const noexcept
    {
        return is_v4() ? v4_.is_multicast() : v6_.is_multicast();
    }

    /** Check if the address is a v4-mapped IPv6 address.

        @return `true` if the address is IPv6 and is an
        IPv4-Mapped IPv6 Address (`::ffff:a.b.c.d`).

        @see
            @ref to_v4.
    */
    bool is_v4_mapped() const noexcept
    {
        return is_v6() && v6_.is_v4_mapped();
    }

    /** Convert to an IPv4 address.

        Returns the held IPv4 address, or the IPv4 address that a
        v4-mapped IPv6 address maps. This makes normalize-then-compare
        a single call when matching addresses across the mapping.

        @throws std::system_error `errc::address_family_not_supported`
        if the address is IPv6 and not v4-mapped.

        @return The IPv4 form of the address.

        @see
            @ref is_v4, @ref is_v4_mapped.
    */
    ipv4_address to_v4() const
    {
        return is_v4() ? v4_ : v6_.to_v4();
    }

    /** Convert to an IPv6 address.

        To map an IPv4 address into IPv6, use the
        `ipv6_address(ipv4_address const&)` constructor instead.

        @throws std::system_error `errc::address_family_not_supported`
        if the address is IPv4.

        @return The held IPv6 address.

        @see
            @ref is_v6.
    */
    ipv6_address to_v6() const
    {
        if (is_v4())
            detail::throw_system_error(
                std::make_error_code(std::errc::address_family_not_supported),
                "address is not IPv6");
        return v6_;
    }

    /** Return the address as a string.

        IPv4 addresses format in dotted decimal, IPv6 addresses
        in standard notation without surrounding brackets.

        @return The address as a string.
    */
    std::string to_string() const
    {
        return is_v4() ? v4_.to_string() : v6_.to_string();
    }

    /** Write a string representing the address to a buffer.

        The resulting buffer is not null-terminated.

        @throws std::length_error `dest_size < ip_address::max_str_len`

        @param dest The buffer in which to write,
        which must have at least `dest_size` space.

        @param dest_size The size of the output buffer.

        @return The formatted string view.
    */
    std::string_view to_buffer(char* dest, std::size_t dest_size) const;

    /** Return true if two addresses are equal.

        Addresses are equal if they have the same family and the
        same value. A v4-mapped IPv6 address is not equal to the
        IPv4 address it maps; normalize with @ref ip_address::to_v4
        to compare
        across the mapping.

        @return `true` if the addresses are equal.
    */
    friend bool operator==(ip_address const& a1, ip_address const& a2) noexcept
    {
        if (a1.family_ != a2.family_)
            return false;
        return a1.is_v4() ? a1.v4_ == a2.v4_ : a1.v6_ == a2.v6_;
    }

    /** Order two addresses.

        Establishes a strict total ordering consistent with
        @ref operator==: addresses are ordered first by family
        (IPv4 before IPv6), then by value. This makes `ip_address`
        usable as a key in ordered containers such as `std::map`
        and `std::set`.

        @return The relative order of `a1` and `a2`.
    */
    friend std::strong_ordering
    operator<=>(ip_address const& a1, ip_address const& a2) noexcept
    {
        if (a1.family_ != a2.family_)
            return a1.is_v4() ? std::strong_ordering::less
                              : std::strong_ordering::greater;
        return a1.is_v4() ? a1.v4_ <=> a2.v4_ : a1.v6_ <=> a2.v6_;
    }

    /** Format the address to an output stream.

        @param os The output stream.
        @param addr The address to format.
        @return The output stream.
    */
    friend BOOST_COROSIO_DECL std::ostream&
    operator<<(std::ostream& os, ip_address const& addr);
};

/** Create an IP address from a string.

    This function parses `s` as an IPv4 address in dotted decimal form, or
    an IPv6 address in hexadecimal notation. An IPv6 address may carry a
    `%zone` suffix: a decimal interface index, or an interface name where
    the platform names interfaces. The string must contain the address
    alone: port suffixes, surrounding brackets, and host names are not
    accepted.

    @par Exception Safety
    Throws nothing.

    @param s The string to parse.
    @return The error code, empty on success, and the parsed
        address — default-constructed on failure.
*/
[[nodiscard]] BOOST_COROSIO_DECL capy::io_result<ip_address>
make_ip_address(std::string_view s) noexcept;

inline ip_address::ip_address(std::string_view s)
{
    auto [ec, addr] = make_ip_address(s);
    if (ec)
        detail::throw_system_error(ec, "invalid IP address");
    *this = addr;
}

} // namespace boost::corosio

namespace std {

/// Hash support for `boost::corosio::ip_address`.
template<>
struct hash<boost::corosio::ip_address>
{
    /// Return the hash of `addr`.
    std::size_t
    operator()(boost::corosio::ip_address const& addr) const noexcept
    {
        // Family-guarded dispatch keeps the throwing conversions
        // unreachable
        return addr.is_v4()
            ? hash<boost::corosio::ipv4_address>()(addr.to_v4())
            : hash<boost::corosio::ipv6_address>()(addr.to_v6());
    }
};

} // namespace std

#endif
