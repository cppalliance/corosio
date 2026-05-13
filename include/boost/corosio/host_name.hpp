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
    Strong guarantee.

    @par Example
    @code
    std::string h = boost::corosio::host_name();
    std::cout << "running on " << h << "\n";
    @endcode

    @return The hostname as a UTF-8 string.

    @throws std::runtime_error If the underlying system call fails.
*/
BOOST_COROSIO_DECL
std::string
host_name();

} // namespace boost::corosio

#endif
