//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/except.hpp>
#include <stdexcept>

namespace boost::corosio::detail {

void throw_logic_error(char const* what)
{
    throw std::logic_error(what);
}

void throw_system_error(std::error_code const& ec)
{
    throw std::system_error(ec);
}

void throw_system_error(
    std::error_code const& ec,
    char const* what)
{
    throw std::system_error(ec, what);
}

} // namespace boost::corosio::detail
