//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EXCEPT_HPP
#define BOOST_COROSIO_DETAIL_EXCEPT_HPP

#include <boost/corosio/detail/config.hpp>
#include <system_error>

namespace boost::corosio::detail {

[[noreturn]] BOOST_COROSIO_DECL void throw_logic_error(char const* what);

[[noreturn]] BOOST_COROSIO_DECL void throw_system_error(std::error_code const& ec);

[[noreturn]] BOOST_COROSIO_DECL void throw_system_error(
    std::error_code const& ec,
    char const* what);

} // namespace boost::corosio::detail

#endif
