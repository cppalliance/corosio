//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/native/native_socket_option.hpp>

#include "test_suite.hpp"

namespace boost::corosio {

struct native_socket_option_test
{
    // getsockopt writes a single byte for boolean/integer options on some
    // platforms; resize() folds it back to a normalized value. Drive
    // resize(1) directly so the single-byte branch is covered on hosts
    // whose getsockopt writes the full width. Mirrors the socket_option
    // twin for the devirtualized option types.
    void testResizeNormalization()
    {
        native_socket_option::no_delay b(true);
        b.resize(1);
        BOOST_TEST(b.value());

        native_socket_option::receive_buffer_size i(1);
        i.resize(1);
        BOOST_TEST_EQ(i.value(), 1);
    }

    void run()
    {
        testResizeNormalization();
    }
};

TEST_SUITE(native_socket_option_test, "boost.corosio.native.socket_option");

} // namespace boost::corosio
