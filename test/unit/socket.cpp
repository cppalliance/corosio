//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/socket.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/async_run.hpp>

#include "test_suite.hpp"

// Global counter for tracking I/O operations
std::size_t g_io_count = 0;

namespace boost {
namespace corosio {

//------------------------------------------------
// Socket-specific tests
// Focus: socket awaitable interface, frame allocator, basic operation
//------------------------------------------------

struct socket_test
{
    // Simple coroutine that performs one async read operation
    static capy::task<>
    read_once(socket& sock)
    {
        co_await sock.async_read_some();
    }

    // Coroutine for multiple sequential reads
    static capy::task<>
    read_multiple(socket& sock, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            co_await sock.async_read_some();
        }
    }

    void
    testSingleLayerCoroutine()
    {
        // Reset I/O counter
        g_io_count = 0;

        // Create I/O context (single-threaded)
        io_context ioc;

        // Create a mock socket
        socket sock(ioc);

        // Get executor
        auto ex = ioc.get_executor();

        // Launch the coroutine
        capy::async_run(ex)(read_once(sock));

        // With inline dispatch, coroutine runs immediately until first I/O suspend
        BOOST_TEST_EQ(g_io_count, 1u);

        // Run the I/O context to process the coroutine completion
        ioc.run();

        // The I/O operation was already started; ioc.run() resumes the coroutine
        BOOST_TEST_EQ(g_io_count, 1u);
    }

    void
    testMultipleReads()
    {
        g_io_count = 0;

        io_context ioc;
        socket sock(ioc);
        auto ex = ioc.get_executor();

        const int read_count = 5;
        capy::async_run(ex)(read_multiple(sock, read_count));

        // With inline dispatch, first I/O is started immediately
        BOOST_TEST_EQ(g_io_count, 1u);

        ioc.run();

        BOOST_TEST_EQ(g_io_count, static_cast<std::size_t>(read_count));
    }

    void
    testMultipleCoroutines()
    {
        g_io_count = 0;

        io_context ioc;
        socket sock1(ioc);
        socket sock2(ioc);
        socket sock3(ioc);
        auto ex = ioc.get_executor();

        // Launch three coroutines
        capy::async_run(ex)(read_once(sock1));
        capy::async_run(ex)(read_once(sock2));
        capy::async_run(ex)(read_once(sock3));

        // With inline dispatch, all 3 coroutines run to first I/O immediately
        BOOST_TEST_EQ(g_io_count, 3u);

        ioc.run();

        // All three should have completed one read each
        BOOST_TEST_EQ(g_io_count, 3u);
    }

    void
    testAlwaysSuspends()
    {
        io_context ioc;
        socket sock(ioc);

        auto awaitable = sock.async_read_some();

        // The awaitable should always suspend (return false from await_ready)
        BOOST_TEST_EQ(awaitable.await_ready(), false);
    }

    void
    run()
    {
        testSingleLayerCoroutine();
        testMultipleReads();
        testMultipleCoroutines();
        testAlwaysSuspends();
    }
};

TEST_SUITE(socket_test, "boost.corosio.socket");

} // namespace corosio
} // namespace boost
