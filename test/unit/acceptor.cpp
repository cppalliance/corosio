//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/acceptor.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

namespace boost::corosio {

//------------------------------------------------
// Acceptor-specific tests
// Focus: acceptor construction, basic interface, and cancellation
//------------------------------------------------

struct acceptor_test
{
    void
    testConstruction()
    {
        io_context ioc;
        acceptor acc(ioc);

        // Acceptor should not be open initially
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void
    testListen()
    {
        io_context ioc;
        acceptor acc(ioc);

        // Listen on a port
        acc.listen(endpoint(0));  // Port 0 = ephemeral port
        BOOST_TEST_EQ(acc.is_open(), true);

        // Close it
        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        acceptor acc1(ioc);
        acc1.listen(endpoint(0));
        BOOST_TEST_EQ(acc1.is_open(), true);

        // Move construct
        acceptor acc2(std::move(acc1));
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        acceptor acc1(ioc);
        acceptor acc2(ioc);
        acc1.listen(endpoint(0));
        BOOST_TEST_EQ(acc1.is_open(), true);
        BOOST_TEST_EQ(acc2.is_open(), false);

        // Move assign
        acc2 = std::move(acc1);
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    //------------------------------------------------
    // Cancellation Tests
    //------------------------------------------------

    void
    testCancelAccept()
    {
        // Tests that cancel() properly cancels a pending accept operation.
        // This exercises the acceptor_ptr shared_ptr that keeps the
        // acceptor impl alive until IOCP delivers the cancellation.
        io_context ioc;
        acceptor acc(ioc);
        acc.listen(endpoint(0));

        // These must outlive the coroutines
        bool accept_done = false;
        system::error_code accept_ec;
        socket peer(ioc);

        auto task = [&]() -> capy::task<>
        {
            // Start a timer to cancel the accept
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

            // Launch accept that will block (no incoming connections)
            // Store lambda in variable to ensure it outlives the coroutine.
            auto nested_coro = [&acc, &peer, &accept_done, &accept_ec]() -> capy::task<>
            {
                auto [ec] = co_await acc.accept(peer);
                accept_ec = ec;
                accept_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait for timer then cancel
            (void)co_await t.wait();
            acc.cancel();

            // Wait for accept to complete
            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(accept_done);
            BOOST_TEST(accept_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        acc.close();
    }

    void
    testCloseWhilePendingAccept()
    {
        // Tests that close() properly handles a pending accept operation.
        // This is the key test for the cancel/destruction race condition:
        // when close() is called, CancelIoEx is invoked, the socket is closed,
        // but the impl must stay alive until IOCP delivers the cancellation.
        // The acceptor_ptr shared_ptr in accept_op ensures this.
        io_context ioc;
        acceptor acc(ioc);
        acc.listen(endpoint(0));

        socket peer(ioc);
        bool accept_done = false;
        system::error_code accept_ec;

        // Pattern from socket tests: run a single coroutine that manages
        // the nested coroutine and close operation
        auto task = [&ioc, &acc, &peer, &accept_done, &accept_ec]() -> capy::task<>
        {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

            // Store lambda in variable to ensure it outlives the coroutine.
            // Lambda coroutines capture 'this' by reference, so the lambda
            // must remain alive while the coroutine is suspended.
            auto nested_coro = [&acc, &peer, &accept_done, &accept_ec]() -> capy::task<>
            {
                auto [ec] = co_await acc.accept(peer);
                accept_ec = ec;
                accept_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait then close the acceptor
            (void)co_await t.wait();
            acc.close();

            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(accept_done);
            BOOST_TEST(accept_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
    }

    void
    run()
    {
        testConstruction();
        testListen();
        testMoveConstruct();
        testMoveAssign();

        // Cancellation
        testCancelAccept();
        testCloseWhilePendingAccept();
    }
};

TEST_SUITE(acceptor_test, "boost.corosio.acceptor");

} // namespace boost::corosio
