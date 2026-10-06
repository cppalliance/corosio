//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/error.hpp>

#include <cstring>
#include <system_error>

#include "test_suite.hpp"

namespace boost::corosio {

struct error_test
{
    void testAlreadyOpen()
    {
        std::error_code ec = error::already_open;
        BOOST_TEST(ec);
        BOOST_TEST(ec == error::already_open);
        BOOST_TEST(std::strcmp(ec.category().name(), "boost.corosio") == 0);
        BOOST_TEST(!ec.message().empty());
        // Distinct from every errc it might be confused with.
        BOOST_TEST(ec != std::errc::already_connected);
        BOOST_TEST(ec != std::errc::invalid_argument);
    }

    void run()
    {
        testAlreadyOpen();
    }
};

TEST_SUITE(error_test, "boost.corosio.error");

} // namespace boost::corosio
