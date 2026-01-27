//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tcp_server.hpp>

#include "test_suite.hpp"

namespace boost::corosio {

struct tcp_server_test
{
    void run()
    {
    }
};

TEST_SUITE(tcp_server_test, "boost.corosio.tcp_server");

} // namespace boost::corosio
