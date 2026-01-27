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
#include <boost/assert/source_location.hpp>
#include <boost/system/error_code.hpp>

namespace boost::corosio::detail {

BOOST_COROSIO_DECL void BOOST_NORETURN throw_logic_error(
    char const* what,
    source_location const& loc = BOOST_CURRENT_LOCATION);

/** Throw a system_error exception.

    @note Callers should check `ec.failed()` before calling this function.
    The inline check is faster than the function call overhead.

    @param ec The error code to throw.
    @param loc Source location for diagnostics.
*/
BOOST_COROSIO_DECL void BOOST_NORETURN throw_system_error(
    system::error_code const& ec,
    source_location const& loc = BOOST_CURRENT_LOCATION);

/** Throw a system_error exception with context.

    @note Callers should check `ec.failed()` before calling this function.
    The inline check is faster than the function call overhead.

    @param ec The error code to throw.
    @param what Context string describing the operation that failed.
    @param loc Source location for diagnostics.
*/
BOOST_COROSIO_DECL void BOOST_NORETURN throw_system_error(
    system::error_code const& ec,
    char const* what,
    source_location const& loc = BOOST_CURRENT_LOCATION);

} // namespace boost::corosio::detail

#endif
