//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/error.hpp>

namespace boost::corosio::detail {

char const*
error_cat_type::name() const noexcept
{
    return "boost.corosio";
}

std::string
error_cat_type::message(int ev) const
{
    switch (static_cast<error>(ev))
    {
    case error::already_open:
        return "already open";
    }
    return "unknown corosio error";
}

#if defined(__cpp_constinit) && __cpp_constinit >= 201907L
constinit error_cat_type error_cat;
#else
error_cat_type error_cat;
#endif

} // namespace boost::corosio::detail
