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

/// Throw `std::logic_error` with a default message.
[[noreturn]] BOOST_COROSIO_DECL void throw_logic_error();

/** Throw `std::logic_error` with the given message.

    @param what Null-terminated message string.

    @throws std::logic_error Always.
*/
[[noreturn]] BOOST_COROSIO_DECL void throw_logic_error(char const* what);

/** Throw `std::system_error` from @p ec.

    @param ec Error code used to construct the exception.

    @throws std::system_error Always.
*/
[[noreturn]] BOOST_COROSIO_DECL void
throw_system_error(std::error_code const& ec);

/** Throw `std::system_error` from @p ec with the given context.

    @param ec Error code used to construct the exception.
    @param what Null-terminated context string.

    @throws std::system_error Always.
*/
[[noreturn]] BOOST_COROSIO_DECL void
throw_system_error(std::error_code const& ec, char const* what);

} // namespace boost::corosio::detail

#endif
