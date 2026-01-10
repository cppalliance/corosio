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
        // Create I/O context (single-threaded)
        io_context ioc;

        // Create a mock socket
        socket sock(ioc);

        // Get executor
        auto ex = ioc.get_executor();

        // Launch the coroutine
        capy::async_run(ex)(read_once(sock));

        // Run the I/O context to process the coroutine completion
        ioc.run();
    }

    void
    testMultipleReads()
    {
        io_context ioc;
        socket sock(ioc);
        auto ex = ioc.get_executor();

        const int read_count = 5;
        capy::async_run(ex)(read_multiple(sock, read_count));

        ioc.run();
    }

    void
    testMultipleCoroutines()
    {
        io_context ioc;
        socket sock1(ioc);
        socket sock2(ioc);
        socket sock3(ioc);
        auto ex = ioc.get_executor();

        // Launch three coroutines
        capy::async_run(ex)(read_once(sock1));
        capy::async_run(ex)(read_once(sock2));
        capy::async_run(ex)(read_once(sock3));

        ioc.run();
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
