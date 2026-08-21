//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_HOST_NAME_HPP
#define BOOST_COROSIO_HOST_NAME_HPP

#include <boost/corosio/detail/config.hpp>

#include <boost/capy/io_result.hpp>

#include <string>

namespace boost::corosio {

/** Return the local machine's hostname.

    On POSIX systems this calls `gethostname(2)`. On Windows this
    calls `GetComputerNameExW(ComputerNameDnsHostname, ...)` and
    converts the result from UTF-16 to UTF-8.

    The function is synchronous and does not require an
    `io_context`. On Windows it does not require winsock to have
    been initialized.

    @par Exception Safety
    Strong guarantee; throws only on allocation failure.

    @par Example
    @code
    auto [ec, h] = boost::corosio::host_name();
    if (ec)
        return;
    std::cout << "running on " << h << "\n";
    @endcode

    @return The error code, empty on success, and the hostname as a
        UTF-8 string — empty on failure.
*/
[[nodiscard]] BOOST_COROSIO_DECL capy::io_result<std::string>
host_name();

} // namespace boost::corosio

#endif
