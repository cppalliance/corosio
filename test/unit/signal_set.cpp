//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/signal_set.hpp>

#ifdef _WIN32

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <csignal>
#include <chrono>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

//------------------------------------------------
// Signal set tests
// Focus: construction, add/remove, wait, and cancellation
//------------------------------------------------

struct signal_set_test
{
    //--------------------------------------------
    // Construction and move semantics
    //--------------------------------------------

    void
    testConstruction()
    {
        io_context ioc;
        signal_set s(ioc);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithOneSignal()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithTwoSignals()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT, SIGTERM);

        BOOST_TEST_PASS();
    }

    void
    testConstructWithThreeSignals()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT, SIGTERM, SIGABRT);

        BOOST_TEST_PASS();
    }

    void
    testMoveConstruct()
    {
        io_context ioc;
        signal_set s1(ioc, SIGINT);

        signal_set s2(std::move(s1));
        BOOST_TEST_PASS();
    }

    void
    testMoveAssign()
    {
        io_context ioc;
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc);

        s2 = std::move(s1);
        BOOST_TEST_PASS();
    }

    void
    testMoveAssignCrossContextThrows()
    {
        io_context ioc1;
        io_context ioc2;
        signal_set s1(ioc1);
        signal_set s2(ioc2);

        BOOST_TEST_THROWS(s2 = std::move(s1), std::logic_error);
    }

    //--------------------------------------------
    // Add/remove/clear tests
    //--------------------------------------------

    void
    testAdd()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        BOOST_TEST_PASS();
    }

    void
    testAddWithErrorCode()
    {
        io_context ioc;
        signal_set s(ioc);

        system::error_code ec;
        s.add(SIGINT, ec);
        BOOST_TEST(!ec);
    }

    void
    testAddDuplicate()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        s.add(SIGINT);  // Should be no-op
        BOOST_TEST_PASS();
    }

    void
    testAddInvalidSignal()
    {
        io_context ioc;
        signal_set s(ioc);

        system::error_code ec;
        s.add(-1, ec);
        BOOST_TEST(ec);
    }

    void
    testRemove()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        s.remove(SIGINT);
        BOOST_TEST_PASS();
    }

    void
    testRemoveWithErrorCode()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        system::error_code ec;
        s.remove(SIGINT, ec);
        BOOST_TEST(!ec);
    }

    void
    testRemoveNotPresent()
    {
        io_context ioc;
        signal_set s(ioc);

        // Removing signal not in set should be a no-op
        s.remove(SIGINT);
        BOOST_TEST_PASS();
    }

    void
    testClear()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        s.add(SIGTERM);
        s.clear();
        BOOST_TEST_PASS();
    }

    void
    testClearWithErrorCode()
    {
        io_context ioc;
        signal_set s(ioc);

        s.add(SIGINT);
        system::error_code ec;
        s.clear(ec);
        BOOST_TEST(!ec);
    }

    void
    testClearEmpty()
    {
        io_context ioc;
        signal_set s(ioc);

        s.clear();  // Should be no-op
        BOOST_TEST_PASS();
    }

    //--------------------------------------------
    // Async wait tests
    //--------------------------------------------

    void
    testWaitWithSignal()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;
        system::error_code result_ec;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s.async_wait();
                result_ec = ec;
                received_signal = signum;
                completed = true;
            }());

        // Raise signal after a short delay
        t.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await t.wait();
                std::raise(SIGINT);
            }());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    void
    testWaitWithDifferentSignal()
    {
        io_context ioc;
        signal_set s(ioc, SIGTERM);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s.async_wait();
                received_signal = signum;
                completed = true;
                (void)ec;
            }());

        t.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await t.wait();
                std::raise(SIGTERM);
            }());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    //--------------------------------------------
    // Cancellation tests
    //--------------------------------------------

    void
    testCancel()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer cancel_timer(ioc);

        bool completed = false;
        system::error_code result_ec;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s.async_wait();
                result_ec = ec;
                completed = true;
                (void)signum;
            }());

        cancel_timer.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await cancel_timer.wait();
                s.cancel();
            }());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testCancelNoWaiters()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);

        s.cancel();  // Should be no-op
        BOOST_TEST_PASS();
    }

    void
    testCancelMultipleTimes()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);

        s.cancel();
        s.cancel();
        s.cancel();
        BOOST_TEST_PASS();
    }

    //--------------------------------------------
    // Multiple signal set tests
    //--------------------------------------------

    void
    testMultipleSignalSetsOnSameSignal()
    {
        io_context ioc;
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc, SIGINT);
        timer t(ioc);

        bool s1_completed = false;
        bool s2_completed = false;
        int s1_signal = 0;
        int s2_signal = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s1.async_wait();
                s1_signal = signum;
                s1_completed = true;
                (void)ec;
            }());

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s2.async_wait();
                s2_signal = signum;
                s2_completed = true;
                (void)ec;
            }());

        t.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await t.wait();
                std::raise(SIGINT);
            }());

        ioc.run();
        BOOST_TEST(s1_completed);
        BOOST_TEST(s2_completed);
        BOOST_TEST_EQ(s1_signal, SIGINT);
        BOOST_TEST_EQ(s2_signal, SIGINT);
    }

    void
    testSignalSetWithMultipleSignals()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT, SIGTERM);
        timer t(ioc);

        bool completed = false;
        int received_signal = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s.async_wait();
                received_signal = signum;
                completed = true;
                (void)ec;
            }());

        // Raise SIGTERM (not SIGINT)
        t.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await t.wait();
                std::raise(SIGTERM);
            }());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    //--------------------------------------------
    // Queued signal tests
    //--------------------------------------------

    void
    testQueuedSignal()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);

        // Raise signal before starting wait
        std::raise(SIGINT);

        bool completed = false;
        int received_signal = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto [ec, signum] = co_await s.async_wait();
                received_signal = signum;
                completed = true;
                (void)ec;
            }());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    //--------------------------------------------
    // Sequential wait tests
    //--------------------------------------------

    void
    testSequentialWaits()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        int wait_count = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                // First wait
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.wait();
                std::raise(SIGINT);

                auto [ec1, sig1] = co_await s.async_wait();
                BOOST_TEST(!ec1);
                BOOST_TEST_EQ(sig1, SIGINT);
                ++wait_count;

                // Second wait
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.wait();
                std::raise(SIGINT);

                auto [ec2, sig2] = co_await s.async_wait();
                BOOST_TEST(!ec2);
                BOOST_TEST_EQ(sig2, SIGINT);
                ++wait_count;
            }());

        ioc.run();
        BOOST_TEST_EQ(wait_count, 2);
    }

    //--------------------------------------------
    // io_result tests
    //--------------------------------------------

    void
    testIoResultSuccess()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        bool result_ok = false;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.wait();
                std::raise(SIGINT);

                auto result = co_await s.async_wait();
                result_ok = static_cast<bool>(result);
            }());

        ioc.run();
        BOOST_TEST(result_ok);
    }

    void
    testIoResultCanceled()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer cancel_timer(ioc);

        bool result_ok = true;
        system::error_code result_ec;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                auto result = co_await s.async_wait();
                result_ok = static_cast<bool>(result);
                result_ec = result.ec;
            }());

        cancel_timer.expires_after(std::chrono::milliseconds(10));
        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                co_await cancel_timer.wait();
                s.cancel();
            }());

        ioc.run();
        BOOST_TEST(!result_ok);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testIoResultStructuredBinding()
    {
        io_context ioc;
        signal_set s(ioc, SIGINT);
        timer t(ioc);

        system::error_code captured_ec;
        int captured_signal = 0;

        capy::run_async(ioc.get_executor())(
            [&]() -> capy::task<>
            {
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.wait();
                std::raise(SIGINT);

                auto [ec, signum] = co_await s.async_wait();
                captured_ec = ec;
                captured_signal = signum;
            }());

        ioc.run();
        BOOST_TEST(!captured_ec);
        BOOST_TEST_EQ(captured_signal, SIGINT);
    }

    void
    run()
    {
        // Construction and move semantics
        testConstruction();
        testConstructWithOneSignal();
        testConstructWithTwoSignals();
        testConstructWithThreeSignals();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContextThrows();

        // Add/remove/clear tests
        testAdd();
        testAddWithErrorCode();
        testAddDuplicate();
        testAddInvalidSignal();
        testRemove();
        testRemoveWithErrorCode();
        testRemoveNotPresent();
        testClear();
        testClearWithErrorCode();
        testClearEmpty();

        // Async wait tests
        testWaitWithSignal();
        testWaitWithDifferentSignal();

        // Cancellation tests
        testCancel();
        testCancelNoWaiters();
        testCancelMultipleTimes();

        // Multiple signal set tests
        testMultipleSignalSetsOnSameSignal();
        testSignalSetWithMultipleSignals();

        // Queued signal tests
        testQueuedSignal();

        // Sequential wait tests
        testSequentialWaits();

        // io_result tests
        testIoResultSuccess();
        testIoResultCanceled();
        testIoResultStructuredBinding();
    }
};

TEST_SUITE(signal_set_test, "boost.corosio.signal_set");

} // namespace corosio
} // namespace boost

#endif // _WIN32
