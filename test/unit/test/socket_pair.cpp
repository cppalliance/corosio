//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

namespace boost::corosio::test {

struct socket_pair_test
{
    void
    testCreate()
    {
        io_context ioc;

        auto [s1, s2] = make_socket_pair(ioc);
        BOOST_TEST(s1.is_open());
        BOOST_TEST(s2.is_open());

        s1.close();
        s2.close();
    }

    void
    testBidirectional()
    {
        io_context ioc;

        auto [s1, s2] = make_socket_pair(ioc);

        auto task = [](socket& a, socket& b) -> capy::task<>
        {
            char buf[32] = {};

            // Write from s1, read from s2
            auto [ec1, n1] = co_await a.write_some(
                capy::const_buffer("hello", 5));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 5u);

            auto [ec2, n2] = co_await b.read_some(
                capy::make_buffer(buf));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n2), "hello");

            // Write from s2, read from s1
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
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();

        s1.close();
        s2.close();
    }

    void
    run()
    {
        testCreate();
        testBidirectional();
    }
};

TEST_SUITE(socket_pair_test, "boost.corosio.socket_pair");

} // namespace boost::corosio::test
