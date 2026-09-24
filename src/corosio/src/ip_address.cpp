//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/ip_address.hpp>

#include <ostream>
#include <stdexcept>

namespace boost::corosio {

std::string_view
ip_address::to_buffer(char* dest, std::size_t dest_size) const
{
    // Uniform capacity requirement: a v4 value in an ip_address
    // still requires the family-independent max_str_len.
    if (dest_size < max_str_len)
        throw std::length_error("buffer too small for IP address");
    return is_v4_ ? v4_.to_buffer(dest, dest_size)
                  : v6_.to_buffer(dest, dest_size);
}

std::ostream&
operator<<(std::ostream& os, ip_address const& addr)
{
    return addr.is_v4_ ? os << addr.v4_ : os << addr.v6_;
}

capy::io_result<ip_address>
make_ip_address(std::string_view s) noexcept
{
    if (auto [ec, v4] = make_ipv4_address(s); !ec)
        return {std::error_code{}, ip_address(v4)};
    auto [ec, v6] = make_ipv6_address(s);
    if (ec)
        return {ec, ip_address{}};
    return {std::error_code{}, ip_address(v6)};
}

} // namespace boost::corosio
