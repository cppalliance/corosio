//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/timer.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

// Include platform-specific context headers for multi-backend testing
#if !defined(_WIN32)
#include <boost/corosio/select_context.hpp>
#endif

#include <chrono>

#include "test_suite.hpp"

namespace boost::corosio {

//------------------------------------------------
// Timer-specific tests
// Focus: timer construction, expiry, wait, and cancellation
//
// Tests are templated on the context type to run with all available backends.
//------------------------------------------------

template<class Context>
struct timer_test_impl
{
    //--------------------------------------------
    // Construction and move semantics
    //--------------------------------------------

    void
    testConstruction()
    {
        Context ioc;
        timer t(ioc);

        BOOST_TEST_PASS();
    }

    void
    testMoveConstruct()
    {
        Context ioc;
        timer t1(ioc);
        t1.expires_after(std::chrono::milliseconds(100));
        auto expiry = t1.expiry();

        timer t2(std::move(t1));
        BOOST_TEST(t2.expiry() == expiry);
    }

    void
    testMoveAssign()
    {
        Context ioc;
        timer t1(ioc);
        timer t2(ioc);

        t1.expires_after(std::chrono::milliseconds(100));
        auto expiry = t1.expiry();

        t2 = std::move(t1);
        BOOST_TEST(t2.expiry() == expiry);
    }

    void
    testMoveAssignCrossContextThrows()
    {
        Context ioc1;
        Context ioc2;
        timer t1(ioc1);
        timer t2(ioc2);

        BOOST_TEST_THROWS(t2 = std::move(t1), std::logic_error);
    }

    //--------------------------------------------
    // Expiry setting and retrieval
    //--------------------------------------------

    void
    testDefaultExpiry()
    {
        Context ioc;
        timer t(ioc);

        auto expiry = t.expiry();
        BOOST_TEST(expiry == timer::time_point{});
    }

    void
    testExpiresAfter()
    {
        Context ioc;
        timer t(ioc);

        auto before = timer::clock_type::now();
        t.expires_after(std::chrono::milliseconds(100));
        auto after = timer::clock_type::now();

        auto expiry = t.expiry();
        BOOST_TEST(expiry >= before + std::chrono::milliseconds(100));
        BOOST_TEST(expiry <= after + std::chrono::milliseconds(100));
    }

    void
    testExpiresAfterDifferentDurations()
    {
        Context ioc;
        timer t(ioc);

        auto before = timer::clock_type::now();
        t.expires_after(std::chrono::seconds(1));
        auto expiry = t.expiry();
        BOOST_TEST(expiry >= before + std::chrono::seconds(1));

        before = timer::clock_type::now();
        t.expires_after(std::chrono::microseconds(500000));
        expiry = t.expiry();
        BOOST_TEST(expiry >= before + std::chrono::microseconds(500000));

        before = timer::clock_type::now();
        t.expires_after(std::chrono::hours(0));
        expiry = t.expiry();
        BOOST_TEST(expiry <= before + std::chrono::milliseconds(10));
    }

    void
    testExpiresAt()
    {
        Context ioc;
        timer t(ioc);

        auto target = timer::clock_type::now() + std::chrono::milliseconds(200);
        t.expires_at(target);

        BOOST_TEST(t.expiry() == target);
    }

    void
    testExpiresAtPast()
    {
        Context ioc;
        timer t(ioc);

        auto target = timer::clock_type::now() - std::chrono::seconds(1);
        t.expires_at(target);

        BOOST_TEST(t.expiry() == target);
    }

    void
    testExpiresAtReplace()
    {
        Context ioc;
        timer t(ioc);

        auto first = timer::clock_type::now() + std::chrono::seconds(10);
        t.expires_at(first);
        BOOST_TEST(t.expiry() == first);

        auto second = timer::clock_type::now() + std::chrono::seconds(5);
        t.expires_at(second);
        BOOST_TEST(t.expiry() == second);
    }

    //--------------------------------------------
    // Async wait tests
    //--------------------------------------------

    void
    testWaitBasic()
    {
        Context ioc;
        timer t(ioc);

        bool completed = false;
        system::error_code result_ec;

        t.expires_after(std::chrono::milliseconds(10));

        auto task = [](timer& t_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void
    testWaitTimingAccuracy()
    {
        Context ioc;
        timer t(ioc);

        auto start = timer::clock_type::now();
        timer::duration elapsed;

        t.expires_after(std::chrono::milliseconds(50));

        auto task = [](timer& t_ref, timer::time_point start_val, timer::duration& elapsed_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            elapsed_out = timer::clock_type::now() - start_val;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t, start, elapsed));

        ioc.run();

        BOOST_TEST(elapsed >= std::chrono::milliseconds(50));
        BOOST_TEST(elapsed < std::chrono::milliseconds(200));
    }

    void
    testWaitExpiredTimer()
    {
        Context ioc;
        timer t(ioc);

        bool completed = false;
        system::error_code result_ec;

        t.expires_at(timer::clock_type::now() - std::chrono::seconds(1));

        auto task = [](timer& t_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void
    testWaitZeroDuration()
    {
        Context ioc;
        timer t(ioc);

        bool completed = false;
        system::error_code result_ec;

        t.expires_after(std::chrono::milliseconds(0));

        auto task = [](timer& t_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    //--------------------------------------------
    // Cancellation tests
    //--------------------------------------------

    void
    testCancel()
    {
        Context ioc;
        timer t(ioc);
        timer cancel_timer(ioc);

        bool completed = false;
        system::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        cancel_timer.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ec, completed));

        auto cancel_task = [](timer& cancel_t_ref, timer& t_ref) -> capy::task<>
        {
            (void)co_await cancel_t_ref.wait();
            t_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testCancelNoWaiters()
    {
        Context ioc;
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));

        t.cancel();
        BOOST_TEST_PASS();
    }

    void
    testCancelMultipleTimes()
    {
        Context ioc;
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));

        t.cancel();
        t.cancel();
        t.cancel();
        BOOST_TEST_PASS();
    }

    void
    testExpiresAtCancelsWaiter()
    {
        Context ioc;
        timer t(ioc);
        timer delay_timer(ioc);

        bool completed = false;
        system::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        delay_timer.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, system::error_code& ec_out, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
            done_out = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ec, completed));

        auto delay_task = [](timer& delay_ref, timer& t_ref) -> capy::task<>
        {
            (void)co_await delay_ref.wait();
            t_ref.expires_after(std::chrono::seconds(30));
        };
        capy::run_async(ioc.get_executor())(delay_task(delay_timer, t));

        ioc.run_for(std::chrono::milliseconds(100));
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    //--------------------------------------------
    // Multiple timer tests
    //--------------------------------------------

    void
    testMultipleTimersDifferentExpiry()
    {
        Context ioc;
        timer t1(ioc);
        timer t2(ioc);
        timer t3(ioc);

        int order = 0;
        int t1_order = 0, t2_order = 0, t3_order = 0;

        t1.expires_after(std::chrono::milliseconds(30));
        t2.expires_after(std::chrono::milliseconds(10));
        t3.expires_after(std::chrono::milliseconds(20));

        auto task = [](timer& t_ref, int& order_ref, int& t_order_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            t_order_out = ++order_ref;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t1, order, t1_order));
        capy::run_async(ioc.get_executor())(task(t2, order, t2_order));
        capy::run_async(ioc.get_executor())(task(t3, order, t3_order));

        ioc.run();

        BOOST_TEST_EQ(t2_order, 1);
        BOOST_TEST_EQ(t3_order, 2);
        BOOST_TEST_EQ(t1_order, 3);
    }

    void
    testMultipleTimersSameExpiry()
    {
        Context ioc;
        timer t1(ioc);
        timer t2(ioc);

        bool t1_done = false, t2_done = false;

        auto expiry = timer::clock_type::now() + std::chrono::milliseconds(20);
        t1.expires_at(expiry);
        t2.expires_at(expiry);

        auto task = [](timer& t_ref, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t1, t1_done));
        capy::run_async(ioc.get_executor())(task(t2, t2_done));

        ioc.run();

        BOOST_TEST(t1_done);
        BOOST_TEST(t2_done);
    }

    //--------------------------------------------
    // Sequential wait tests
    //--------------------------------------------

    void
    testSequentialWaits()
    {
        Context ioc;
        timer t(ioc);

        int wait_count = 0;

        auto task = [](timer& t_ref, int& count_out) -> capy::task<>
        {
            t_ref.expires_after(std::chrono::milliseconds(5));
            auto [ec1] = co_await t_ref.wait();
            BOOST_TEST(!ec1);
            ++count_out;

            t_ref.expires_after(std::chrono::milliseconds(5));
            auto [ec2] = co_await t_ref.wait();
            BOOST_TEST(!ec2);
            ++count_out;

            t_ref.expires_after(std::chrono::milliseconds(5));
            auto [ec3] = co_await t_ref.wait();
            BOOST_TEST(!ec3);
            ++count_out;
        };
        capy::run_async(ioc.get_executor())(task(t, wait_count));

        ioc.run();
        BOOST_TEST_EQ(wait_count, 3);
    }

    //--------------------------------------------
    // io_result tests
    //--------------------------------------------

    void
    testIoResultSuccess()
    {
        Context ioc;
        timer t(ioc);

        bool result_ok = false;

        t.expires_after(std::chrono::milliseconds(5));

        auto task = [](timer& t_ref, bool& ok_out) -> capy::task<>
        {
            auto result = co_await t_ref.wait();
            ok_out = !result.ec;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ok));

        ioc.run();
        BOOST_TEST(result_ok);
    }

    void
    testIoResultCanceled()
    {
        Context ioc;
        timer t(ioc);
        timer cancel_timer(ioc);

        bool result_ok = true;
        system::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        cancel_timer.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, bool& ok_out, system::error_code& ec_out) -> capy::task<>
        {
            auto result = co_await t_ref.wait();
            ok_out = !result.ec;
            ec_out = result.ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ok, result_ec));

        auto cancel_task = [](timer& cancel_t_ref, timer& t_ref) -> capy::task<>
        {
            (void)co_await cancel_t_ref.wait();
            t_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, t));

        ioc.run();
        BOOST_TEST(!result_ok);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void
    testIoResultStructuredBinding()
    {
        Context ioc;
        timer t(ioc);

        system::error_code captured_ec;

        t.expires_after(std::chrono::milliseconds(5));

        auto task = [](timer& t_ref, system::error_code& ec_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            ec_out = ec;
        };
        capy::run_async(ioc.get_executor())(task(t, captured_ec));

        ioc.run();
        BOOST_TEST(!captured_ec);
    }

    //--------------------------------------------
    // Edge cases
    //--------------------------------------------

    void
    testLongDuration()
    {
        Context ioc;
        timer t(ioc);

        t.expires_after(std::chrono::hours(24 * 365));

        auto expiry = t.expiry();
        BOOST_TEST(expiry > timer::clock_type::now());

        t.cancel();
        BOOST_TEST_PASS();
    }

    void
    testNegativeDuration()
    {
        Context ioc;
        timer t(ioc);

        bool completed = false;

        t.expires_after(std::chrono::milliseconds(-100));

        auto task = [](timer& t_ref, bool& done_out) -> capy::task<>
        {
            auto [ec] = co_await t_ref.wait();
            done_out = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t, completed));

        ioc.run();
        BOOST_TEST(completed);
    }

    //--------------------------------------------
    // Type trait tests
    //--------------------------------------------

    void
    testTypeAliases()
    {
        static_assert(std::is_same_v<
            timer::clock_type,
            std::chrono::steady_clock>);

        static_assert(std::is_same_v<
            timer::time_point,
            std::chrono::steady_clock::time_point>);

        static_assert(std::is_same_v<
            timer::duration,
            std::chrono::steady_clock::duration>);

        BOOST_TEST_PASS();
    }

    void
    run()
    {
        // Construction and move semantics
        testConstruction();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContextThrows();

        // Expiry setting and retrieval
        testDefaultExpiry();
        testExpiresAfter();
        testExpiresAfterDifferentDurations();
        testExpiresAt();
        testExpiresAtPast();
        testExpiresAtReplace();

        // Async wait tests
        testWaitBasic();
        testWaitTimingAccuracy();
        testWaitExpiredTimer();
        testWaitZeroDuration();

        // Cancellation tests
        testCancel();
        testCancelNoWaiters();
        testCancelMultipleTimes();
        testExpiresAtCancelsWaiter();

        // Multiple timer tests
        testMultipleTimersDifferentExpiry();
        testMultipleTimersSameExpiry();

        // Sequential wait tests
        testSequentialWaits();

        // io_result tests
        testIoResultSuccess();
        testIoResultCanceled();
        testIoResultStructuredBinding();

        // Edge cases
        testLongDuration();
        testNegativeDuration();

        // Type trait tests
        testTypeAliases();
    }
};

//------------------------------------------------
// Register test suites for each available backend
//------------------------------------------------

// Default io_context (platform default backend)
struct timer_test : timer_test_impl<io_context> {};
TEST_SUITE(timer_test, "boost.corosio.timer");

// POSIX: also test with select_context explicitly
#if !defined(_WIN32)
struct timer_test_select : timer_test_impl<select_context> {};
TEST_SUITE(timer_test_select, "boost.corosio.timer.select");
#endif

} // namespace boost::corosio
