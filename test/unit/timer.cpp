//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/timer.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <memory>
#include <new>
#include <stop_token>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Timer-specific tests
// Focus: timer construction, expiry, wait, and cancellation
//
// Tests are templated on the context type to run with all available backends.

template<auto Backend>
struct timer_test
{
    // Construction and move semantics

    void testConstruction()
    {
        io_context ioc(Backend);
        timer t(ioc);

        BOOST_TEST_PASS();
    }

    void testConstructionWithTimePoint()
    {
        io_context ioc(Backend);
        auto tp = timer::clock_type::now() + std::chrono::seconds(10);
        timer t(ioc, tp);

        BOOST_TEST(t.expiry() == tp);
    }

    void testConstructionWithDuration()
    {
        io_context ioc(Backend);
        auto before = timer::clock_type::now();
        timer t(ioc, std::chrono::milliseconds(500));
        auto after = timer::clock_type::now();

        BOOST_TEST(t.expiry() >= before + std::chrono::milliseconds(500));
        BOOST_TEST(t.expiry() <= after + std::chrono::milliseconds(500));
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        timer t1(ioc);
        t1.expires_after(std::chrono::milliseconds(100));
        auto expiry = t1.expiry();

        timer t2(std::move(t1));
        BOOST_TEST(t2.expiry() == expiry);
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        timer t1(ioc);
        timer t2(ioc);

        t1.expires_after(std::chrono::milliseconds(100));
        auto expiry = t1.expiry();

        t2 = std::move(t1);
        BOOST_TEST(t2.expiry() == expiry);
    }

    void testMoveAssignCrossContext()
    {
        io_context ioc1(Backend);
        io_context ioc2(Backend);
        timer t1(ioc1);
        timer t2(ioc2);

        t1.expires_after(std::chrono::milliseconds(100));
        auto expiry = t1.expiry();

        t2 = std::move(t1);
        BOOST_TEST(t2.expiry() == expiry);
    }

    // Expiry setting and retrieval

    void testDefaultExpiry()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto expiry = t.expiry();
        BOOST_TEST(expiry == timer::time_point{});
    }

    void testExpiresAfter()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto before = timer::clock_type::now();
        t.expires_after(std::chrono::milliseconds(100));
        auto after = timer::clock_type::now();

        auto expiry = t.expiry();
        BOOST_TEST(expiry >= before + std::chrono::milliseconds(100));
        BOOST_TEST(expiry <= after + std::chrono::milliseconds(100));
    }

    void testExpiresAfterDifferentDurations()
    {
        io_context ioc(Backend);
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

    void testExpiresAt()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto target = timer::clock_type::now() + std::chrono::milliseconds(200);
        t.expires_at(target);

        BOOST_TEST(t.expiry() == target);
    }

    void testExpiresAtPast()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto target = timer::clock_type::now() - std::chrono::seconds(1);
        t.expires_at(target);

        BOOST_TEST(t.expiry() == target);
    }

    void testExpiresAtReplace()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto first = timer::clock_type::now() + std::chrono::seconds(10);
        t.expires_at(first);
        BOOST_TEST(t.expiry() == first);

        auto second = timer::clock_type::now() + std::chrono::seconds(5);
        t.expires_at(second);
        BOOST_TEST(t.expiry() == second);
    }

    // Async wait tests

    void testWaitBasic()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool completed = false;
        std::error_code result_ec;

        t.expires_after(std::chrono::milliseconds(10));

        auto task = [](timer& t_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testWaitTimingAccuracy()
    {
        io_context ioc(Backend);
        timer t(ioc);

        auto start = timer::clock_type::now();
        timer::duration elapsed;

        t.expires_after(std::chrono::milliseconds(50));

        auto task = [](timer& t_ref, timer::time_point start_val,
                       timer::duration& elapsed_out) -> capy::task<> {
            auto [ec]   = co_await t_ref.wait();
            elapsed_out = timer::clock_type::now() - start_val;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t, start, elapsed));

        ioc.run();

        BOOST_TEST(elapsed >= std::chrono::milliseconds(50));
        BOOST_TEST(elapsed < std::chrono::seconds(2));
    }

    void testWaitExpiredTimer()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool completed = false;
        std::error_code result_ec;

        t.expires_at(timer::clock_type::now() - std::chrono::seconds(1));

        auto task = [](timer& t_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testWaitZeroDuration()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool completed = false;
        std::error_code result_ec;

        t.expires_after(std::chrono::milliseconds(0));

        auto task = [](timer& t_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ec, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    // Cancellation tests

    void testCancel()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer cancel_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        cancel_timer.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ec, completed));

        auto cancel_task = [](timer& cancel_t_ref,
                              timer& t_ref) -> capy::task<> {
            (void)co_await cancel_t_ref.wait();
            t_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testCancelNoWaiters()
    {
        io_context ioc(Backend);
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));

        t.cancel();
        BOOST_TEST_PASS();
    }

    void testCancelMultipleTimes()
    {
        io_context ioc(Backend);
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));

        t.cancel();
        t.cancel();
        t.cancel();
        BOOST_TEST_PASS();
    }

    void testExpiresAtCancelsWaiter()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer delay_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        delay_timer.expires_after(std::chrono::milliseconds(50));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ec, completed));

        auto delay_task = [](timer& delay_ref, timer& t_ref) -> capy::task<> {
            (void)co_await delay_ref.wait();
            t_ref.expires_after(std::chrono::seconds(30));
        };
        capy::run_async(ioc.get_executor())(delay_task(delay_timer, t));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testStopTokenCancellation()
    {
        // A pending timer wait should be cancelled when its stop_token
        // is signaled after the wait has already suspended.
        io_context ioc(Backend);
        timer t(ioc);
        timer delay(ioc);

        std::stop_source stop_src;
        bool wait_done    = false;
        bool failsafe_hit = false;
        std::error_code wait_ec;

        t.expires_after(std::chrono::seconds(60));

        // Waiter task — bound to stop_token
        auto wait_task = [&]() -> capy::task<> {
            auto [ec] = co_await t.wait();
            wait_ec   = ec;
            wait_done = true;
        };

        // Canceller — short delay then signal stop
        auto canceller_task = [&]() -> capy::task<> {
            delay.expires_after(std::chrono::milliseconds(10));
            (void)co_await delay.wait();
            stop_src.request_stop();
        };

        // Failsafe — if stop_token didn't cancel the timer,
        // fall back to manual cancel so the test doesn't hang
        auto failsafe_task = [&]() -> capy::task<> {
            timer ft(ioc);
            ft.expires_after(std::chrono::milliseconds(1000));
            auto [ec] = co_await ft.wait();
            if (!ec && !wait_done)
            {
                failsafe_hit = true;
                t.cancel();
            }
        };

        capy::run_async(ioc.get_executor(), stop_src.get_token())(wait_task());
        capy::run_async(ioc.get_executor())(canceller_task());
        capy::run_async(ioc.get_executor())(failsafe_task());

        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);

        // If the failsafe was hit, stop_token cancellation didn't work —
        // only the manual t.cancel() fallback rescued the test.
        BOOST_TEST(!failsafe_hit);
    }

    // Multiple timer tests

    void testMultipleTimersDifferentExpiry()
    {
        io_context ioc(Backend);
        timer t1(ioc);
        timer t2(ioc);
        timer t3(ioc);

        int order    = 0;
        int t1_order = 0, t2_order = 0, t3_order = 0;

        t1.expires_after(std::chrono::milliseconds(30));
        t2.expires_after(std::chrono::milliseconds(10));
        t3.expires_after(std::chrono::milliseconds(20));

        auto task = [](timer& t_ref, int& order_ref,
                       int& t_order_out) -> capy::task<> {
            auto [ec]   = co_await t_ref.wait();
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

    void testMultipleTimersSameExpiry()
    {
        io_context ioc(Backend);
        timer t1(ioc);
        timer t2(ioc);

        bool t1_done = false, t2_done = false;

        auto expiry = timer::clock_type::now() + std::chrono::milliseconds(20);
        t1.expires_at(expiry);
        t2.expires_at(expiry);

        auto task = [](timer& t_ref, bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            done_out  = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t1, t1_done));
        capy::run_async(ioc.get_executor())(task(t2, t2_done));

        ioc.run();

        BOOST_TEST(t1_done);
        BOOST_TEST(t2_done);
    }

    void testReheapifyOnExpiresAtUpdate()
    {
        // Push several waiters into the heap, suspend them via run_for so
        // their wait actually registers, then move the first timer's
        // expiry far into the future — exercises the update_timer
        // re-heap (down_heap) branch in timer_service.
        io_context ioc(Backend);
        timer t1(ioc), t2(ioc), t3(ioc), t4(ioc);

        bool d1 = false, d2 = false, d3 = false, d4 = false;
        std::error_code e1, e2, e3, e4;

        t1.expires_after(std::chrono::seconds(5));
        t2.expires_after(std::chrono::milliseconds(20));
        t3.expires_after(std::chrono::milliseconds(30));
        t4.expires_after(std::chrono::milliseconds(40));

        auto task = [](timer& t, bool& done, std::error_code& ec_out)
            -> capy::task<> {
            auto [ec] = co_await t.wait();
            done = true;
            ec_out = ec;
        };

        capy::run_async(ioc.get_executor())(task(t1, d1, e1));
        capy::run_async(ioc.get_executor())(task(t2, d2, e2));
        capy::run_async(ioc.get_executor())(task(t3, d3, e3));
        capy::run_async(ioc.get_executor())(task(t4, d4, e4));

        // Let the waiters actually register in the heap, then move
        // t1's expiry far past t2..t4 — this cancels t1's pending
        // waiter and exercises the down-heap branch.
        ioc.poll(); // process any inline work
        ioc.restart();
        t1.expires_at(timer::clock_type::now() + std::chrono::seconds(60));

        ioc.run();

        BOOST_TEST(d2);
        BOOST_TEST(d3);
        BOOST_TEST(d4);
        BOOST_TEST(!e2);
        BOOST_TEST(!e3);
        BOOST_TEST(!e4);
        // t1's waiter was cancelled by the second expires_at.
        BOOST_TEST(d1);
        BOOST_TEST(e1 == capy::cond::canceled);
    }

    void testTimerFreeListReuseAcrossContexts()
    {
        // Create timers in one context, destroy the context, then create
        // a new context — covers the timer_service shutdown free-list
        // delete path and the construct() free-list pop path.
        for (int i = 0; i < 3; ++i)
        {
            io_context ioc(Backend);
            timer t1(ioc), t2(ioc), t3(ioc);
            t1.expires_after(std::chrono::milliseconds(1));
            t2.expires_after(std::chrono::milliseconds(2));
            t3.expires_after(std::chrono::milliseconds(3));

            bool done = false;
            auto task = [&]() -> capy::task<> {
                (void)co_await t1.wait();
                (void)co_await t2.wait();
                (void)co_await t3.wait();
                done = true;
            };
            capy::run_async(ioc.get_executor())(task());
            ioc.run();
            BOOST_TEST(done);
        }
    }

    // Multiple waiters on one timer

    void testMultipleWaiters()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool w1 = false, w2 = false, w3 = false;
        std::error_code ec1, ec2, ec3;

        t.expires_after(std::chrono::milliseconds(10));

        auto task = [](timer& t_ref, std::error_code& ec_out,
                       bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        capy::run_async(ioc.get_executor())(task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(task(t, ec2, w2));
        capy::run_async(ioc.get_executor())(task(t, ec3, w3));

        ioc.run();

        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(w3);
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);
        BOOST_TEST(!ec3);
    }

    void testMultipleWaitersCancelAll()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer delay(ioc);

        bool w1 = false, w2 = false, w3 = false;
        std::error_code ec1, ec2, ec3;

        t.expires_after(std::chrono::seconds(60));
        delay.expires_after(std::chrono::milliseconds(10));

        auto task = [](timer& t_ref, std::error_code& ec_out,
                       bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        auto cancel_task = [](timer& delay_ref, timer& t_ref) -> capy::task<> {
            (void)co_await delay_ref.wait();
            t_ref.cancel();
        };

        capy::run_async(ioc.get_executor())(task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(task(t, ec2, w2));
        capy::run_async(ioc.get_executor())(task(t, ec3, w3));
        capy::run_async(ioc.get_executor())(cancel_task(delay, t));

        ioc.run();

        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(w3);
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(ec2 == capy::cond::canceled);
        BOOST_TEST(ec3 == capy::cond::canceled);
    }

    void testMultipleWaitersStopTokenCancelsOne()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer delay(ioc);

        std::stop_source stop_src;
        bool w1 = false, w2 = false;
        std::error_code ec1, ec2;

        t.expires_after(std::chrono::milliseconds(500));
        delay.expires_after(std::chrono::milliseconds(10));

        // w1 has a stop_token — will be cancelled individually
        auto wait_task = [&]() -> capy::task<> {
            auto [ec] = co_await t.wait();
            ec1       = ec;
            w1        = true;
        };

        // w2 has no stop_token — completes when timer fires
        auto wait_task2 = [&]() -> capy::task<> {
            auto [ec] = co_await t.wait();
            ec2       = ec;
            w2        = true;
        };

        auto cancel_one = [&]() -> capy::task<> {
            (void)co_await delay.wait();
            stop_src.request_stop();
        };

        capy::run_async(ioc.get_executor(), stop_src.get_token())(wait_task());
        capy::run_async(ioc.get_executor())(wait_task2());
        capy::run_async(ioc.get_executor())(cancel_one());

        ioc.run();

        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(!ec2);
    }

    // Destruction cancels pending waiters

    void testDestructionCancelsPendingWaiters()
    {
        io_context ioc(Backend);
        timer delay(ioc);

        bool w1 = false, w2 = false;
        std::error_code ec1, ec2;

        auto t = std::make_unique<timer>(ioc);
        t->expires_after(std::chrono::seconds(60));

        delay.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        auto destroy_task = [&]() -> capy::task<> {
            (void)co_await delay.wait();
            t.reset();
        };

        capy::run_async(ioc.get_executor())(wait_task(*t, ec1, w1));
        capy::run_async(ioc.get_executor())(wait_task(*t, ec2, w2));
        capy::run_async(ioc.get_executor())(destroy_task());

        ioc.run();

        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(ec2 == capy::cond::canceled);
    }

    // cancel_one() tests

    void testCancelOne()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer delay(ioc);

        bool w1 = false, w2 = false;
        std::error_code ec1, ec2;

        t.expires_after(std::chrono::milliseconds(500));
        delay.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        auto cancel_one_task = [](timer& delay_ref,
                                  timer& t_ref) -> capy::task<> {
            (void)co_await delay_ref.wait();
            auto n = t_ref.cancel_one();
            BOOST_TEST_EQ(n, 1u);
        };

        capy::run_async(ioc.get_executor())(wait_task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(wait_task(t, ec2, w2));
        capy::run_async(ioc.get_executor())(cancel_one_task(delay, t));

        ioc.run();

        BOOST_TEST(w1);
        BOOST_TEST(w2);
        // First waiter (FIFO) is cancelled, second fires normally
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(!ec2);
    }

    void testCancelOneNoWaiters()
    {
        io_context ioc(Backend);
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));

        auto n = t.cancel_one();
        BOOST_TEST_EQ(n, 0u);
    }

    // Return value tests

    void testCancelReturnsCount()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer delay(ioc);

        bool w1 = false, w2 = false, w3 = false;
        std::error_code ec1, ec2, ec3;

        t.expires_after(std::chrono::seconds(60));
        delay.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        std::size_t cancel_count = 0;
        auto cancel_task = [&](timer& delay_ref, timer& t_ref) -> capy::task<> {
            (void)co_await delay_ref.wait();
            cancel_count = t_ref.cancel();
        };

        capy::run_async(ioc.get_executor())(wait_task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(wait_task(t, ec2, w2));
        capy::run_async(ioc.get_executor())(wait_task(t, ec3, w3));
        capy::run_async(ioc.get_executor())(cancel_task(delay, t));

        ioc.run();

        BOOST_TEST_EQ(cancel_count, 3u);
        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(w3);
    }

    void testCancelReturnsZeroNoWaiters()
    {
        io_context ioc(Backend);
        timer t(ioc);

        t.expires_after(std::chrono::seconds(60));
        auto n = t.cancel();
        BOOST_TEST_EQ(n, 0u);
    }

    void testExpiresAtReturnsCount()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool w1 = false, w2 = false;
        std::error_code ec1, ec2;

        t.expires_after(std::chrono::seconds(60));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        capy::run_async(ioc.get_executor())(wait_task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(wait_task(t, ec2, w2));

        // Drain so both waiters suspend on t.wait() and register.
        while (ioc.poll() > 0)
            ;

        auto expires_count = t.expires_at(
            timer::clock_type::now() + std::chrono::seconds(30));

        // Drain so the canceled completions reach the coroutines.
        while (ioc.poll() > 0)
            ;

        BOOST_TEST_EQ(expires_count, 2u);
        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(ec2 == capy::cond::canceled);
    }

    void testExpiresAfterReturnsCount()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool w1 = false, w2 = false;
        std::error_code ec1, ec2;

        t.expires_after(std::chrono::seconds(60));

        auto wait_task = [](timer& t_ref, std::error_code& ec_out,
                            bool& done) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done      = true;
        };

        capy::run_async(ioc.get_executor())(wait_task(t, ec1, w1));
        capy::run_async(ioc.get_executor())(wait_task(t, ec2, w2));

        // Drain so both waiters suspend on t.wait() and register.
        while (ioc.poll() > 0)
            ;

        auto expires_count = t.expires_after(std::chrono::seconds(30));

        // Drain so the canceled completions reach the coroutines.
        while (ioc.poll() > 0)
            ;

        BOOST_TEST_EQ(expires_count, 2u);
        BOOST_TEST(w1);
        BOOST_TEST(w2);
        BOOST_TEST(ec1 == capy::cond::canceled);
        BOOST_TEST(ec2 == capy::cond::canceled);
    }

    // Sequential wait tests

    void testSequentialWaits()
    {
        io_context ioc(Backend);
        timer t(ioc);

        int wait_count = 0;

        auto task = [](timer& t_ref, int& count_out) -> capy::task<> {
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

    // io_result tests

    void testIoResultSuccess()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool result_ok = false;

        t.expires_after(std::chrono::milliseconds(5));

        auto task = [](timer& t_ref, bool& ok_out) -> capy::task<> {
            auto result = co_await t_ref.wait();
            ok_out      = !result.ec;
        };
        capy::run_async(ioc.get_executor())(task(t, result_ok));

        ioc.run();
        BOOST_TEST(result_ok);
    }

    void testIoResultCanceled()
    {
        io_context ioc(Backend);
        timer t(ioc);
        timer cancel_timer(ioc);

        bool result_ok = true;
        std::error_code result_ec;

        t.expires_after(std::chrono::seconds(60));
        cancel_timer.expires_after(std::chrono::milliseconds(10));

        auto wait_task = [](timer& t_ref, bool& ok_out,
                            std::error_code& ec_out) -> capy::task<> {
            auto result = co_await t_ref.wait();
            ok_out      = !result.ec;
            ec_out      = result.ec;
        };
        capy::run_async(ioc.get_executor())(wait_task(t, result_ok, result_ec));

        auto cancel_task = [](timer& cancel_t_ref,
                              timer& t_ref) -> capy::task<> {
            (void)co_await cancel_t_ref.wait();
            t_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(cancel_timer, t));

        ioc.run();
        BOOST_TEST(!result_ok);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testIoResultStructuredBinding()
    {
        io_context ioc(Backend);
        timer t(ioc);

        std::error_code captured_ec;

        t.expires_after(std::chrono::milliseconds(5));

        auto task = [](timer& t_ref, std::error_code& ec_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
        };
        capy::run_async(ioc.get_executor())(task(t, captured_ec));

        ioc.run();
        BOOST_TEST(!captured_ec);
    }

    // Shutdown cleanup

    void testShutdownDestroysTimerWaiters()
    {
        bool started   = false;
        bool destroyed = false;

        {
            io_context ioc(Backend);
            timer t(ioc);
            t.expires_after(std::chrono::seconds(3600));

            auto task = [](timer& t_ref, bool& started_flag,
                           bool& destroyed_flag) -> capy::task<> {
                struct guard
                {
                    bool& flag_;
                    ~guard()
                    {
                        flag_ = true;
                    }
                };
                guard g{destroyed_flag};
                started_flag = true;
                auto [ec]    = co_await t_ref.wait();
                (void)ec;
            };

            capy::run_async(ioc.get_executor())(task(t, started, destroyed));
            ioc.poll();

            BOOST_TEST(started);
            // io_context destructor triggers shutdown
        }

        BOOST_TEST(destroyed);
    }

    void testShutdownDrainsHeapWaiters()
    {
        // Exercises timer_service::shutdown()'s waiter drain loop.
        // Normally the timer destructs before io_context, cancelling
        // waiters via cancel_timer(). Here we use placement-new so the
        // timer outlives io_context — its destructor is skipped, leaving
        // waiters in the heap for shutdown() to drain.
        int destroyed = 0;

        {
            io_context ioc(Backend);

            alignas(timer) unsigned char buf[sizeof(timer)];
            auto* t = new (buf) timer(ioc);
            t->expires_after(std::chrono::seconds(3600));

            auto task = [](timer& t_ref, int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard()
                    {
                        ++c_;
                    }
                };
                guard g{counter};
                auto [ec] = co_await t_ref.wait();
                (void)ec;
            };

            capy::run_async(ioc.get_executor())(task(*t, destroyed));
            ioc.poll();

            // io_context destructs. Timer t is still alive in buf.
            // timer_service::shutdown() finds the waiter in the heap
            // and drains it, destroying the coroutine frame.
            // Timer destructor is intentionally skipped (placement-new).
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    void testAbruptStopWithPendingTimerOps()
    {
        bool waiter_started = false;

        {
            io_context ioc(Backend);
            timer t1(ioc);
            timer t2(ioc);
            timer t3(ioc);

            t1.expires_after(std::chrono::hours(1));
            t2.expires_after(std::chrono::hours(1));
            t3.expires_after(std::chrono::hours(1));

            auto waiter = [](timer& t, bool& started) -> capy::task<> {
                started   = true;
                auto [ec] = co_await t.wait();
                (void)ec;
            };

            auto stopper = [](io_context& ctx) -> capy::task<> {
                ctx.stop();
                co_return;
            };

            capy::run_async(ioc.get_executor())(waiter(t1, waiter_started));
            capy::run_async(ioc.get_executor())(waiter(t2, waiter_started));
            capy::run_async(ioc.get_executor())(waiter(t3, waiter_started));
            capy::run_async(ioc.get_executor())(stopper(ioc));

            ioc.run();

            BOOST_TEST(waiter_started);
            // io_context destructs with 3 pending timer waiters
        }
        // Shutdown completes without hanging
        BOOST_TEST_PASS();
    }

    void testShutdownWithTimerOwnedByCoroutine()
    {
        // Reproduces UB: timer_service::shutdown() iterates heap_,
        // calls h.destroy() on a waiter. The coroutine frame owns
        // a timer whose destructor re-enters destroy_impl() →
        // cancel_timer() → remove_timer_impl(), modifying heap_
        // during the range-for iteration.
        //
        // All timers must be owned by coroutines (not on the stack)
        // so that their heap entries survive until shutdown().
        int destroyed = 0;

        {
            io_context ioc(Backend);

            auto owning_task = [](io_context& ctx,
                                  int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard()
                    {
                        ++c_;
                    }
                };
                guard g{counter};
                timer t(ctx);
                t.expires_after(std::chrono::hours(1));
                auto [ec] = co_await t.wait();
                (void)ec;
            };

            auto stopper = [](io_context& ctx) -> capy::task<> {
                ctx.stop();
                co_return;
            };

            capy::run_async(ioc.get_executor())(owning_task(ioc, destroyed));
            capy::run_async(ioc.get_executor())(owning_task(ioc, destroyed));
            capy::run_async(ioc.get_executor())(owning_task(ioc, destroyed));
            capy::run_async(ioc.get_executor())(stopper(ioc));

            ioc.run();
            // ~io_context → timer_service::shutdown() iterates heap_
            // with 3 entries. Destroying the first coroutine handle
            // triggers its timer destructor which removes an entry
            // via remove_timer_impl(), corrupting the iteration.
        }

        BOOST_TEST_EQ(destroyed, 3);
    }

    // Edge cases

    void testLongDuration()
    {
        io_context ioc(Backend);
        timer t(ioc);

        t.expires_after(std::chrono::hours(24 * 365));

        auto expiry = t.expiry();
        BOOST_TEST(expiry > timer::clock_type::now());

        t.cancel();
        BOOST_TEST_PASS();
    }

    void testNegativeDuration()
    {
        io_context ioc(Backend);
        timer t(ioc);

        bool completed = false;

        t.expires_after(std::chrono::milliseconds(-100));

        auto task = [](timer& t_ref, bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            done_out  = true;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(task(t, completed));

        ioc.run();
        BOOST_TEST(completed);
    }

    // Type trait tests

    void testTypeAliases()
    {
        static_assert(
            std::is_same_v<timer::clock_type, std::chrono::steady_clock>);

        static_assert(
            std::is_same_v<
                timer::time_point, std::chrono::steady_clock::time_point>);

        static_assert(std::is_same_v<
                      timer::duration, std::chrono::steady_clock::duration>);

        BOOST_TEST_PASS();
    }

    void run()
    {
        // Construction and move semantics
        testConstruction();
        testConstructionWithTimePoint();
        testConstructionWithDuration();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContext();

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
        testStopTokenCancellation();

        // Multiple timer tests
        testMultipleTimersDifferentExpiry();
        testMultipleTimersSameExpiry();
        testReheapifyOnExpiresAtUpdate();
        testTimerFreeListReuseAcrossContexts();

        // Multiple waiters on one timer
        testMultipleWaiters();
        testMultipleWaitersCancelAll();
        testMultipleWaitersStopTokenCancelsOne();

        // Destruction cancels pending waiters
        testDestructionCancelsPendingWaiters();

        // cancel_one() tests
        testCancelOne();
        testCancelOneNoWaiters();

        // Return value tests
        testCancelReturnsCount();
        testCancelReturnsZeroNoWaiters();
        testExpiresAtReturnsCount();
        testExpiresAfterReturnsCount();

        // Sequential wait tests
        testSequentialWaits();

        // io_result tests
        testIoResultSuccess();
        testIoResultCanceled();
        testIoResultStructuredBinding();

        // Shutdown cleanup
        testShutdownDestroysTimerWaiters();
        testShutdownDrainsHeapWaiters();
        testAbruptStopWithPendingTimerOps();
        testShutdownWithTimerOwnedByCoroutine();

        // Edge cases
        testLongDuration();
        testNegativeDuration();

        // Type trait tests
        testTypeAliases();
    }
};

COROSIO_BACKEND_TESTS(timer_test, "boost.corosio.timer")

} // namespace boost::corosio
