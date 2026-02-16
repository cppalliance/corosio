//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
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

// Mocket-specific tests

struct mocket_test
{
    void testProvideExpect()
    {
        io_context ioc;
        capy::test::fuse f;

        // Create mocket paired with a socket
        auto [m, peer] = make_mocket_pair(ioc, f);
        BOOST_TEST(m.is_open());
        BOOST_TEST(peer.is_open());

        // Stage data for mocket's reads (from provide buffer)
        m.provide("staged_read_data");

        // Set expectation for mocket's writes
        m.expect("expected_write");

        auto task = [](mocket& m_ref) -> capy::task<> {
            char buf[32] = {};

            // Mocket reads from its own provide buffer
            auto [ec1, n1] = co_await m_ref.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "staged_read_data");

            // Mocket write validates against expect buffer
            auto [ec2, n2] = co_await m_ref.write_some(
                capy::const_buffer("expected_write", 14));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 14u);
        };
        capy::run_async(ioc.get_executor())(task(m));

        ioc.run();
        ioc.restart();

        // All staged data should be consumed
        BOOST_TEST(!m.close());
        peer.close();
    }

    void testCloseWithUnconsumedData()
    {
        io_context ioc;
        capy::test::fuse f;

        auto [m, peer] = make_mocket_pair(ioc, f);

        // Set expectation that won't be fulfilled
        m.expect("never_written");

        // Close should fail because expect_ is not empty
        auto ec = m.close();
        BOOST_TEST(ec == capy::error::test_failure);

        peer.close();
    }

    void testCloseWithUnconsumedProvide()
    {
        io_context ioc;
        capy::test::fuse f;

        auto [m, peer] = make_mocket_pair(ioc, f);

        // Stage data that won't be consumed
        m.provide("never_read");

        // Close should fail because provide_ is not empty
        auto ec = m.close();
        BOOST_TEST(ec == capy::error::test_failure);

        peer.close();
    }

    void testPassthrough()
    {
        io_context ioc;
        capy::test::fuse f;

        auto [m, peer] = make_mocket_pair(ioc, f);

        // Test passthrough when provide/expect buffers are empty
        auto task = [](mocket& m_ref, tcp_socket& peer_ref) -> capy::task<> {
            char buf[32] = {};

            // Write from mocket, read from peer
            auto [ec1, n1] =
                co_await m_ref.write_some(capy::const_buffer("hello", 5));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 5u);

            auto [ec2, n2] =
                co_await peer_ref.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n2), "hello");

            // Write from peer, read from mocket
            auto [ec3, n3] =
                co_await peer_ref.write_some(capy::const_buffer("world", 5));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(n3, 5u);

            auto [ec4, n4] = co_await m_ref.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec4);
            BOOST_TEST_EQ(n4, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n4), "world");
        };
        capy::run_async(ioc.get_executor())(task(m, peer));

        ioc.run();

        BOOST_TEST(!m.close());
        peer.close();
    }

    void run()
    {
        testProvideExpect();
        testCloseWithUnconsumedData();
        testCloseWithUnconsumedProvide();
        testPassthrough();
    }
};

TEST_SUITE(mocket_test, "boost.corosio.mocket");

} // namespace boost::corosio::test
