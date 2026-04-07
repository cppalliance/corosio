//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/detail/except.hpp>

#include <cstring>
#include <ostream>
#include <system_error>

namespace boost::corosio {

local_endpoint::local_endpoint(std::string_view path)
{
    if (path.size() > max_path_length)
        detail::throw_system_error(
            std::make_error_code(std::errc::filename_too_long),
            "local_endpoint");
    std::memcpy(path_, path.data(), path.size());
    len_ = static_cast<std::uint8_t>(path.size());
}

local_endpoint::local_endpoint(
    std::string_view path, std::error_code& ec) noexcept
{
    if (path.size() > max_path_length)
    {
        ec = std::make_error_code(std::errc::filename_too_long);
        return;
    }
    ec = {};
    std::memcpy(path_, path.data(), path.size());
    len_ = static_cast<std::uint8_t>(path.size());
}

std::ostream&
operator<<(std::ostream& os, local_endpoint const& ep)
{
    if (ep.empty())
        return os;
    if (ep.is_abstract())
    {
        // Skip the leading null byte; print the rest as the name
        os << "[abstract:"
           << std::string_view(ep.path_ + 1, ep.len_ - 1)
           << ']';
    }
    else
    {
        os << ep.path();
    }
    return os;
}

} // namespace boost::corosio
