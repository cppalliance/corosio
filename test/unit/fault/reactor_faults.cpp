//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "reactor_faults.hpp"

namespace boost::corosio::test::fault {

COROSIO_REACTOR_BACKEND_TESTS(
    reactor_common_faults, "boost.corosio.fault.reactor");

} // boost::corosio::test::fault
