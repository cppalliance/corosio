//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/test/mocket.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/fuse.hpp>

#include "test_suite.hpp"

namespace boost::corosio::test {

//------------------------------------------------
// Mocket-specific tests
//------------------------------------------------

struct mocket_test
{
    void
    testComprehensive()
    {
        io_context ioc;
        capy::test::fuse f;

        // Test 1: Create mockets and verify they're open
        auto [m1, m2] = make_mockets(ioc, f);
        BOOST_TEST(m1.is_open());
        BOOST_TEST(m2.is_open());

        // Test 2: Stage data and run read/write operations
        m1.provide("hello_from_m1");
        m2.provide("hello_from_m2");
        m1.expect("write_to_m1");
        m2.expect("write_to_m2");

        // Note: Pass captures as parameters to store them in the coroutine frame,
        // avoiding use-after-scope when the lambda temporary is destroyed.
        auto task = [](mocket& m1_ref, mocket& m2_ref) -> capy::task<>
        {
            char buf[32] = {};

            // m2 reads from m1's provide
            auto [ec1, n1] = co_await m2_ref.read_some(
                capy::make_buffer(buf));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "hello_from_m1");

            // m1 reads from m2's provide
            auto [ec2, n2] = co_await m1_ref.read_some(
                capy::make_buffer(buf));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(std::string_view(buf, n2), "hello_from_m2");

            // Write to m1's expect
            auto [ec3, n3] = co_await m1_ref.write_some(
                capy::const_buffer("write_to_m1", 11));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(n3, 11u);

            // Write to m2's expect
            auto [ec4, n4] = co_await m2_ref.write_some(
                capy::const_buffer("write_to_m2", 11));
            BOOST_TEST(!ec4);
            BOOST_TEST_EQ(n4, 11u);
        };
        capy::run_async(ioc.get_executor())(task(m1, m2));

        ioc.run();
        ioc.restart();

        // All staged data should be consumed
        BOOST_TEST(!m1.close());
        BOOST_TEST(!m2.close());
    }

    void
    testCloseWithUnconsumedData()
    {
        io_context ioc;
        capy::test::fuse f;

        auto [m1, m2] = make_mockets(ioc, f);

        // Set expectation that won't be fulfilled
        m2.expect("never_written");

        // Close should fail because expect_ is not empty
        auto ec = m2.close();
        BOOST_TEST(ec == capy::error::test_failure);

        // m1's provide is empty, should succeed
        BOOST_TEST(!m1.close());
    }

    void
    testPassthrough()
    {
        io_context ioc;
        capy::test::fuse f;

        auto [m1, m2] = make_mockets(ioc, f);

        auto task = [](mocket& a, mocket& b) -> capy::task<>
        {
            char buf[32] = {};

            // Write from m1, read from m2
            auto [ec1, n1] = co_await a.write_some(
                capy::const_buffer("hello", 5));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 5u);

            auto [ec2, n2] = co_await b.read_some(
                capy::make_buffer(buf));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n2), "hello");

            // Write from m2, read from m1
            auto [ec3, n3] = co_await b.write_some(
                capy::const_buffer("world", 5));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(n3, 5u);

            auto [ec4, n4] = co_await a.read_some(
                capy::make_buffer(buf));
            BOOST_TEST(!ec4);
            BOOST_TEST_EQ(n4, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n4), "world");
        };
        capy::run_async(ioc.get_executor())(task(m1, m2));

        ioc.run();

        BOOST_TEST(!m1.close());
        BOOST_TEST(!m2.close());
    }

    void
    run()
    {
        testComprehensive();
        testCloseWithUnconsumedData();
        testPassthrough();
    }
};

TEST_SUITE(mocket_test, "boost.corosio.mocket");

} // namespace boost::corosio::test
