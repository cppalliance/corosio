//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/except.hpp>
#include <boost/system/system_error.hpp>
#include <boost/throw_exception.hpp>
#include <stdexcept>

namespace boost::corosio::detail {

void throw_logic_error(
    char const* what,
    source_location const& loc)
{
    throw_exception(std::logic_error(what), loc);
}

void throw_system_error(
    system::error_code const& ec,
    source_location const& loc)
{
    throw_exception(system::system_error(ec), loc);
}

void throw_system_error(
    system::error_code const& ec,
    char const* what,
    source_location const& loc)
{
    throw_exception(system::system_error(ec, what), loc);
}

} // namespace boost::corosio::detail
