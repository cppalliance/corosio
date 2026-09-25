//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/signal_set.hpp>

#include <boost/corosio/delay.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <tuple>

#include "context.hpp"
#include "test_suite.hpp"

#if BOOST_COROSIO_POSIX
#include <signal.h>
#endif

namespace boost::corosio {

// Signal set tests
// Focus: construction, add/remove, wait, and cancellation
//
// Tests are templated on the context type to run with all available backends.

template<auto Backend>
struct signal_set_test
{
    // Construction and move semantics

    void testConstruction()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST_PASS();
    }

    void testConstructWithOneSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        BOOST_TEST_PASS();
    }

    void testConstructWithTwoSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT, SIGTERM);

        BOOST_TEST_PASS();
    }

    void testConstructWithThreeSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT, SIGTERM, SIGABRT);

        BOOST_TEST_PASS();
    }

    void testConstructFromExecutor()
    {
        io_context ioc(Backend);
        signal_set s(ioc.get_executor());

        BOOST_TEST_PASS();
    }

    void testConstructFromExecutorWithSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc.get_executor(), SIGINT, SIGTERM);

        BOOST_TEST_PASS();
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        signal_set s1(ioc, SIGINT);

        signal_set s2(std::move(s1));
        BOOST_TEST_PASS();
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc);

        s2 = std::move(s1);
        BOOST_TEST_PASS();
    }

    void testMoveAssignCrossContext()
    {
        io_context ioc1(Backend);
        io_context ioc2(Backend);
        signal_set s1(ioc1);
        signal_set s2(ioc2);

        s2 = std::move(s1);
        BOOST_TEST_PASS();
    }

    // Add/remove/clear tests

    void testAdd()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        auto result = s.add(SIGINT);
        BOOST_TEST(!result);
    }

    void testAddDuplicate()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(!s.add(SIGINT));
        auto result = s.add(SIGINT); // Should be no-op
        BOOST_TEST(!result);
    }

    void testAddInvalidSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        auto result = s.add(-1);
        BOOST_TEST(!!result);
    }

    void testAddInvalidLargeSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // A signal number above the service's table size returns
        // invalid_argument.
        BOOST_TEST(s.add(100000) == std::errc::invalid_argument);
    }

    void testRemoveInvalidSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(s.remove(-1) == std::errc::invalid_argument);
    }

    void testRemove()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(!s.add(SIGINT));
        auto result = s.remove(SIGINT);
        BOOST_TEST(!result);
    }

    void testRemoveNotPresent()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Removing signal not in set should be a no-op
        auto result = s.remove(SIGINT);
        BOOST_TEST(!result);
    }

    void testClear()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(!s.add(SIGINT));
        BOOST_TEST(!s.add(SIGTERM));
        BOOST_TEST(!s.clear());
    }

    void testClearEmpty()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(!s.clear()); // Should be no-op
    }

    // Async wait tests

    void testWaitWithSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        bool completed      = false;
        int received_signal = 0;
        std::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, std::error_code& ec_out,
                            int& sig_out, bool& done_out) -> capy::task<> {
            auto [ec, signum] = co_await s_ref.wait();
            ec_out            = ec;
            sig_out           = signum;
            done_out          = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, result_ec, received_signal, completed));

        // Raise signal after a short delay
        auto raise_task = []() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    void testWaitWithDifferentSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGTERM);

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            sig_out                            = signum;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, received_signal, completed));

        auto raise_task = []() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGTERM);
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    // Cancellation tests

    void testCancel()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        bool completed = false;
        std::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, std::error_code& ec_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            ec_out                             = ec;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ec, completed));

        auto cancel_task = [](signal_set& s_ref) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            s_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(s));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testCancelBeforeWait()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        bool completed = false;
        std::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, std::error_code& ec_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            ec_out                             = ec;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ec, completed));

        // Cancel before io_context::run() — coroutine hasn't reached wait() yet
        s.cancel();

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testCancelNoWaiters()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        s.cancel(); // Should be no-op
        BOOST_TEST_PASS();
    }

    void testCancelMultipleTimes()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        s.cancel();
        s.cancel();
        s.cancel();
        BOOST_TEST_PASS();
    }

    void testWaitWithPreStoppedToken()
    {
        // A wait() under a stop_token that's already in the requested state
        // completes with capy::error::canceled via the stop_requested branch.
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::stop_source src;
        src.request_stop();

        bool completed = false;
        std::error_code result_ec;

        auto wait_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s.wait();
            result_ec                          = ec;
            completed                          = true;
        };
        capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testMidWaitStopCancellation()
    {
        // A stop request that lands while the wait is already pending must
        // cancel it, the same as every other wait operation in the library.
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::stop_source src;
        bool completed = false;
        std::error_code result_ec;

        auto wait_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s.wait();
            result_ec                          = ec;
            completed                          = true;
        };
        capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

        // Delay so the wait is genuinely parked in the service before the
        // stop lands; a stop requested before wait() begins takes the
        // existing stop_requested() short-circuit instead, which
        // testWaitWithPreStoppedToken already covers.
        auto stop_task = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            src.request_stop();
            co_return;
        };
        capy::run_async(ioc.get_executor())(stop_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testStopAfterSignalDelivered()
    {
        // Review Focus 1: a stop firing after the wait already completed
        // must not poison the next wait on the same set. The poisoning
        // this guards against -- token_cancelled_ left true by a late
        // fire against waiting_ == false -- is only observable on a
        // SECOND wait, so this drives one after the late stop lands.
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::stop_source src;
        bool first_done          = false;
        std::error_code first_ec = capy::error::canceled;

        auto wait_task = [&]() -> capy::task<> {
            auto [ec, signum] = co_await s.wait();
            (void)signum;
            first_ec   = ec;
            first_done = true;
        };
        capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

        auto raise_task = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
            co_return;
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(first_done);
        BOOST_TEST(!first_ec);

        // Ordered after the first wait is observed complete, not after a
        // fixed delay: the callback is still armed here, so this is the
        // late fire against waiting_ == false.
        src.request_stop();

        // reset_token_cancel() must clear token_cancelled_ on the next
        // wait() rather than leaving it to poison this one.
        ioc.restart();
        std::error_code second_ec = capy::error::canceled;
        bool second_done          = false;

        auto second = [&]() -> capy::task<> {
            auto [ec, signum] = co_await s.wait();
            (void)signum;
            second_ec   = ec;
            second_done = true;
        };
        capy::run_async(ioc.get_executor())(second());

        auto raiser = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
            co_return;
        };
        capy::run_async(ioc.get_executor())(raiser());
        ioc.run();

        BOOST_TEST(second_done);
        BOOST_TEST(!second_ec);
    }

    void testWaitAgainAfterStopCancel()
    {
        // Review Focus 4: the token path must not set the sticky
        // `cancelled_` latch, so a second wait still works.
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::stop_source src;
        std::error_code first_ec;
        bool first_done = false;

        auto first = [&]() -> capy::task<> {
            auto [ec, signum] = co_await s.wait();
            (void)signum;
            first_ec   = ec;
            first_done = true;
        };
        capy::run_async(ioc.get_executor(), src.get_token())(first());

        auto stopper = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            src.request_stop();
            co_return;
        };
        capy::run_async(ioc.get_executor())(stopper());
        ioc.run();

        BOOST_TEST(first_done);
        BOOST_TEST(first_ec == capy::cond::canceled);

        ioc.restart();
        std::error_code second_ec = capy::error::canceled;
        bool second_done          = false;

        auto second = [&]() -> capy::task<> {
            auto [ec, signum] = co_await s.wait();
            (void)signum;
            second_ec   = ec;
            second_done = true;
        };
        capy::run_async(ioc.get_executor())(second());

        auto raiser = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
            co_return;
        };
        capy::run_async(ioc.get_executor())(raiser());
        ioc.run();

        BOOST_TEST(second_done);
        BOOST_TEST(!second_ec);
    }

    void testNormalDeliveryWithLiveToken()
    {
        // Review Focus 5: arming a callback for a token that is never
        // requested must not disturb normal delivery.
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::stop_source src;
        bool completed            = false;
        std::error_code result_ec = capy::error::canceled;
        int got                   = 0;

        auto wait_task = [&]() -> capy::task<> {
            auto [ec, signum] = co_await s.wait();
            result_ec         = ec;
            got               = signum;
            completed         = true;
        };
        capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

        auto raiser = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
            co_return;
        };
        capy::run_async(ioc.get_executor())(raiser());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST(got == SIGINT);
    }

    void testDestroyWithArmedStopToken()
    {
        // Review Focus 3: destroying the set while a callback is armed
        // must not leave a dangling `this`. Under ASAN this is the test
        // that catches a missing disarm_stop().
        io_context ioc(Backend);
        std::stop_source src;
        bool completed = false;

        {
            signal_set s(ioc, SIGINT);

            auto wait_task = [&]() -> capy::task<> {
                [[maybe_unused]] auto [ec, signum] = co_await s.wait();
                completed                          = true;
            };
            capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

            auto canceller = [&]() -> capy::task<> {
                std::ignore =
                    co_await corosio::delay(std::chrono::milliseconds(10));
                s.cancel();
                co_return;
            };
            capy::run_async(ioc.get_executor())(canceller());
            ioc.run();
        }

        src.request_stop();
        BOOST_TEST(completed);
    }

    // The token_cancelled_ flag exists for a stop request that lands
    // between wait() arming the callback and start_wait taking the
    // service mutex; without it that wait parks forever. The window is
    // unreachable from the io thread, so drive request_stop() from a
    // second one. Whether any given iteration lands inside the window is
    // timing, so assert only what holds either way: the wait always
    // completes, and always with a cancellation.
    void testConcurrentStopRequestRace()
    {
        constexpr int iterations = 400;

        // Spin first: the window is nanoseconds wide and a yield
        // overshoots it. Yield after that so an oversubscribed machine
        // does not burn a timeslice per iteration. Bounded either way so
        // a regression still terminates.
        auto spin_until = [](std::atomic<bool> const& flag) {
            for (int spin = 0; spin < 10000; ++spin)
                if (flag.load(std::memory_order_acquire))
                    return;
            for (int n = 0; n < 100000; ++n)
            {
                if (flag.load(std::memory_order_acquire))
                    return;
                std::this_thread::yield();
            }
        };

        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        for (int i = 0; i < iterations; ++i)
        {
            std::stop_source src;
            std::atomic<bool> stopper_ready{false};
            std::atomic<bool> arming{false};
            bool completed            = false;
            std::error_code result_ec = capy::error::eof;

            auto wait_task = [&]() -> capy::task<> {
                // Hand off to an already-spinning stopper so thread
                // startup latency does not swamp the stagger below.
                spin_until(stopper_ready);
                arming.store(true, std::memory_order_release);
                [[maybe_unused]] auto [ec, signum] = co_await s.wait();
                result_ec                          = ec;
                completed                          = true;
            };
            capy::run_async(ioc.get_executor(), src.get_token())(wait_task());

            std::thread stopper([&] {
                stopper_ready.store(true, std::memory_order_release);
                spin_until(arming);
                // Walk the offset across iterations: a fixed delay would
                // sit on the same side of the window every time.
                for (int spin = i % 256; spin > 0; --spin)
                    (void)arming.load(std::memory_order_relaxed);
                src.request_stop();
            });

            ioc.restart();
            // Bounded so a lost wakeup is the red assertion below rather
            // than a suite timeout.
            std::ignore = ioc.run_for(std::chrono::seconds(5));
            stopper.join();

            BOOST_TEST(completed);
            BOOST_TEST(result_ec == capy::cond::canceled);
            if (!completed)
                return; // a wait still parked would hang every iteration after
        }
    }

    void testShutdownWithPendingSignalSet()
    {
        // A set destroyed in the documented order gives its registrations
        // back through destroy(), so the service's shutdown finds an
        // empty impl_list_ here; the walk itself is reached by
        // testShutdownReleasesRegistration below.
        [[maybe_unused]] int destroyed = 0;

        {
            io_context ioc(Backend);
            [[maybe_unused]] signal_set s(ioc, SIGINT, SIGTERM);
        }
        BOOST_TEST_PASS();
    }

    // The process signal table outlives every io_context, so a set still
    // registered when its context shuts down -- which an abandoned frame
    // is the only way to arrange -- has to hand the registration back
    // there: a stale entry keeps the signal installed with the old flags
    // and refuses the next add() of it.
    void testShutdownReleasesRegistration()
    {
#if BOOST_COROSIO_POSIX
        constexpr auto parked_flags = signal_set::restart;
        constexpr auto reuse_flags  = signal_set::no_defer;
#else
        constexpr auto parked_flags = signal_set::none;
        constexpr auto reuse_flags  = signal_set::none;
#endif
        bool resumed = false;
        {
            io_context ioc(Backend);
            auto keeper = [&]() -> capy::task<> {
                signal_set sig(ioc);
                BOOST_TEST(!sig.add(SIGINT, parked_flags));
                std::ignore = co_await sig.wait();
                resumed     = true;
            };
            capy::run_async(ioc.get_executor())(keeper());
            // Exactly one handler, the coroutine start: anything else
            // would leave the wait unparked and the set unregistered by
            // the time the context is destroyed.
            BOOST_TEST(ioc.run_one() == 1);
        }
        BOOST_TEST(!resumed);

        io_context ioc(Backend);
        signal_set s(ioc);
        auto add_ec = s.add(SIGINT, reuse_flags);
        BOOST_TEST(!add_ec);
        // Raising with no handler installed would kill the process.
        if (add_ec)
            return;

        // Raised before the wait starts, so nothing here is timed.
        std::raise(SIGINT);

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            auto [ec, signum] = co_await s_ref.wait();
            sig_out           = signum;
            done_out          = !ec;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, received_signal, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    // The walk gives back one entry, not the signal: with a second
    // context registered on SIGINT the disposition has to survive the
    // first context's teardown, and the survivor has to keep receiving.
    void testShutdownKeepsOtherContextRegistered()
    {
        io_context survivor(Backend);
        signal_set kept(survivor);
        BOOST_TEST(!kept.add(SIGINT));

        bool resumed = false;
        {
            io_context ioc(Backend);
            auto keeper = [&]() -> capy::task<> {
                signal_set sig(ioc);
                BOOST_TEST(!sig.add(SIGINT));
                std::ignore = co_await sig.wait();
                resumed     = true;
            };
            capy::run_async(ioc.get_executor())(keeper());
            BOOST_TEST(ioc.run_one() == 1);
        }
        BOOST_TEST(!resumed);

#if BOOST_COROSIO_POSIX
        // A disposition restored out from under the survivor would make
        // the raise below kill the process, so say so as a failure.
        struct sigaction cur = {};
        BOOST_TEST(::sigaction(SIGINT, nullptr, &cur) == 0);
        BOOST_TEST(cur.sa_handler != SIG_DFL);
        if (cur.sa_handler == SIG_DFL)
            return;
#endif

        // Raised before the wait starts, so nothing here is timed.
        std::raise(SIGINT);

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            auto [ec, signum] = co_await s_ref.wait();
            sig_out           = signum;
            done_out          = !ec;
        };
        capy::run_async(survivor.get_executor())(
            wait_task(kept, received_signal, completed));

        survivor.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    // Multiple signal set tests

    void testMultipleSignalSetsOnSameSignal()
    {
        io_context ioc(Backend);
        signal_set s1(ioc, SIGINT);
        signal_set s2(ioc, SIGINT);

        bool s1_completed = false;
        bool s2_completed = false;
        int s1_signal     = 0;
        int s2_signal     = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            sig_out                            = signum;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s1, s1_signal, s1_completed));
        capy::run_async(ioc.get_executor())(
            wait_task(s2, s2_signal, s2_completed));

        auto raise_task = []() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(s1_completed);
        BOOST_TEST(s2_completed);
        BOOST_TEST_EQ(s1_signal, SIGINT);
        BOOST_TEST_EQ(s2_signal, SIGINT);
    }

    void testSignalSetWithMultipleSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT, SIGTERM);

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            sig_out                            = signum;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, received_signal, completed));

        // Raise SIGTERM (not SIGINT)
        auto raise_task = []() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGTERM);
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGTERM);
    }

    // Queued signal tests

    void testQueuedSignal()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        // Raise signal before starting wait
        std::raise(SIGINT);

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            sig_out                            = signum;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, received_signal, completed));

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

    // Sequential wait tests

    void testSequentialWaits()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        int wait_count = 0;

        auto task = [](signal_set& s_ref, int& count_out) -> capy::task<> {
            // First wait
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(5));
            std::raise(SIGINT);

            auto [ec1, sig1] = co_await s_ref.wait();
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(sig1, SIGINT);
            ++count_out;

            // Second wait
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(5));
            std::raise(SIGINT);

            auto [ec2, sig2] = co_await s_ref.wait();
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(sig2, SIGINT);
            ++count_out;
        };
        capy::run_async(ioc.get_executor())(task(s, wait_count));

        ioc.run();
        BOOST_TEST_EQ(wait_count, 2);
    }

    // Async-signal-safety / self-pipe regression tests
    //
    // Signals are delivered via a self-pipe: the handler only write()s the
    // signal number to a pipe and the event loop drains it. These exercise
    // the drain path repeatedly to catch a broken re-arm (io_uring multishot),
    // failure to re-park (epoll/kqueue edge-triggered), or a missed drain
    // (select level-triggered).

    void testManySequentialSignalCycles()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        constexpr int cycles = 100;
        int delivered        = 0;

        auto task = [](signal_set& s_ref, int& count_out) -> capy::task<> {
            for (int i = 0; i < cycles; ++i)
            {
                std::raise(SIGINT);
                auto [ec, signum] = co_await s_ref.wait();
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(signum, SIGINT);
                ++count_out;
            }
        };
        capy::run_async(ioc.get_executor())(task(s, delivered));

        ioc.run();
        BOOST_TEST_EQ(delivered, cycles);
    }

    // A signal that arrives while a coroutine is NOT waiting must still be
    // delivered on the next wait(). Repeat to exercise the queued path (the
    // undelivered counter) alongside the self-pipe drain across many cycles.
    void testInterleavedQueuedAndWaitedSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        int delivered = 0;

        auto task = [](signal_set& s_ref, int& count_out) -> capy::task<> {
            for (int i = 0; i < 20; ++i)
            {
                // Raise before waiting: exercises the queued (undelivered)
                // path where deliver_signal runs with no waiter registered.
                std::raise(SIGINT);
                auto [ec1, sig1] = co_await s_ref.wait();
                BOOST_TEST(!ec1);
                BOOST_TEST_EQ(sig1, SIGINT);
                ++count_out;

                // Raise after a delay while waiting: exercises the live-waiter
                // path where the drain posts a completion.
                std::ignore = co_await delay(std::chrono::milliseconds(1));
                std::raise(SIGINT);
                auto [ec2, sig2] = co_await s_ref.wait();
                BOOST_TEST(!ec2);
                BOOST_TEST_EQ(sig2, SIGINT);
                ++count_out;
            }
        };
        capy::run_async(ioc.get_executor())(task(s, delivered));

        ioc.run();
        BOOST_TEST_EQ(delivered, 40);
    }

    // io_result tests

    void testIoResultSuccess()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        bool result_ok = false;

        auto task = [](signal_set& s_ref, bool& ok_out) -> capy::task<> {
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(5));
            std::raise(SIGINT);

            auto result = co_await s_ref.wait();
            ok_out      = !std::get<0>(result);
        };
        capy::run_async(ioc.get_executor())(task(s, result_ok));

        ioc.run();
        BOOST_TEST(result_ok);
    }

    void testIoResultCanceled()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        bool result_ok = true;
        std::error_code result_ec;

        auto wait_task = [](signal_set& s_ref, bool& ok_out,
                            std::error_code& ec_out) -> capy::task<> {
            auto result = co_await s_ref.wait();
            ok_out      = !std::get<0>(result);
            ec_out      = std::get<0>(result);
        };
        capy::run_async(ioc.get_executor())(wait_task(s, result_ok, result_ec));

        auto cancel_task = [](signal_set& s_ref) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            s_ref.cancel();
        };
        capy::run_async(ioc.get_executor())(cancel_task(s));

        ioc.run();
        BOOST_TEST(!result_ok);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testIoResultStructuredBinding()
    {
        io_context ioc(Backend);
        signal_set s(ioc, SIGINT);

        std::error_code captured_ec;
        int captured_signal = 0;

        auto task = [](signal_set& s_ref, std::error_code& ec_out,
                       int& sig_out) -> capy::task<> {
            std::ignore = co_await corosio::delay(std::chrono::milliseconds(5));
            std::raise(SIGINT);

            auto [ec, signum] = co_await s_ref.wait();
            ec_out            = ec;
            sig_out           = signum;
        };
        capy::run_async(ioc.get_executor())(
            task(s, captured_ec, captured_signal));

        ioc.run();
        BOOST_TEST(!captured_ec);
        BOOST_TEST_EQ(captured_signal, SIGINT);
    }

    // Signal flags tests (cross-platform)

    void testFlagsBitwiseOperations()
    {
        // Test OR
        auto combined = signal_set::restart | signal_set::no_defer;
        BOOST_TEST((combined & signal_set::restart) != signal_set::none);
        BOOST_TEST((combined & signal_set::no_defer) != signal_set::none);
        BOOST_TEST((combined & signal_set::no_child_stop) == signal_set::none);

        // Test compound assignment
        auto flags = signal_set::none;
        flags |= signal_set::restart;
        BOOST_TEST((flags & signal_set::restart) != signal_set::none);

        // Test NOT
        auto all_but_restart = ~signal_set::restart;
        BOOST_TEST((all_but_restart & signal_set::restart) == signal_set::none);
    }

    void testAddWithNoneFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with none (default behavior) - works on all platforms
        auto result = s.add(SIGINT, signal_set::none);
        BOOST_TEST(!result);
    }

    void testAddWithDontCareFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with dont_care - works on all platforms
        auto result = s.add(SIGINT, signal_set::dont_care);
        BOOST_TEST(!result);
    }

    void testRemoveOneOfTwoSignals()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        BOOST_TEST(!s.add(SIGINT));
        BOOST_TEST(!s.add(SIGTERM));
        // Removing the higher signal number walks the sorted per-set
        // list past the lower one.
        BOOST_TEST(!s.remove(SIGTERM));
        BOOST_TEST(!s.remove(SIGINT));
    }

    void testTwoSetsSameSignalRemoveInBothOrders()
    {
        io_context ioc(Backend);
        {
            signal_set s1(ioc), s2(ioc);
            BOOST_TEST(!s1.add(SIGINT));
            BOOST_TEST(!s2.add(SIGINT));
            BOOST_TEST(!s1.remove(SIGINT));
            BOOST_TEST(!s2.remove(SIGINT));
        }
        {
            signal_set s1(ioc), s2(ioc);
            BOOST_TEST(!s1.add(SIGINT));
            BOOST_TEST(!s2.add(SIGINT));
            BOOST_TEST(!s2.remove(SIGINT));
            BOOST_TEST(!s1.remove(SIGINT));
        }
        {
            // clear() walks the same per-signal table links.
            signal_set s1(ioc), s2(ioc);
            BOOST_TEST(!s1.add(SIGINT));
            BOOST_TEST(!s2.add(SIGINT));
            BOOST_TEST(!s1.clear());
            BOOST_TEST(!s2.clear());
        }
    }

    void testSignalDeliveredBeforeWait()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Two sets on one signal, only one waiting: delivery posts to
        // the waiter and queues on the idle registration, whose later
        // wait must consume the queued signal immediately.
        signal_set s1(ioc), s2(ioc);
        BOOST_TEST(!s1.add(SIGINT));
        BOOST_TEST(!s2.add(SIGINT));

        int got1   = 0;
        auto wait1 = [&]() -> capy::task<> {
            auto [ec, sig] = co_await s1.wait();
            if (!ec)
                got1 = sig;
        };
        capy::run_async(ex)(wait1());
        std::raise(SIGINT);
        ioc.run();
        ioc.restart();
        BOOST_TEST_EQ(got1, SIGINT);

        int got2   = 0;
        auto wait2 = [&]() -> capy::task<> {
            auto [ec, sig] = co_await s2.wait();
            if (!ec)
                got2 = sig;
        };
        capy::run_async(ex)(wait2());
        ioc.run();

        BOOST_TEST_EQ(got2, SIGINT);
    }

    void testTwoServicesDestroyInBothOrders()
    {
        // Two io_contexts give the process-wide service list two
        // entries; destroying them in each order exercises both
        // unlink shapes. Each set dies before its own context.
        {
            std::optional<io_context> a(std::in_place, Backend);
            std::optional<io_context> b(std::in_place, Backend);
            std::optional<signal_set> sa(std::in_place, *a);
            std::optional<signal_set> sb(std::in_place, *b);
            BOOST_TEST(!sa->add(SIGINT));
            BOOST_TEST(!sb->add(SIGINT));
            sa.reset();
            a.reset();
            sb.reset();
            b.reset();
        }
        {
            std::optional<io_context> a(std::in_place, Backend);
            std::optional<io_context> b(std::in_place, Backend);
            std::optional<signal_set> sa(std::in_place, *a);
            std::optional<signal_set> sb(std::in_place, *b);
            BOOST_TEST(!sa->add(SIGINT));
            BOOST_TEST(!sb->add(SIGINT));
            sb.reset();
            b.reset();
            sa.reset();
            a.reset();
        }
    }

#if BOOST_COROSIO_POSIX
    // Signal flags tests (POSIX only)
    // Windows returns operation_not_supported for
    // flags other than none/dont_care

    void testAddWithChildAndResetFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Never raised here: only the sigaction flag translation is
        // under test.
        BOOST_TEST(!s.add(
            SIGCHLD, signal_set::no_child_stop | signal_set::no_child_wait));
        BOOST_TEST(!s.remove(SIGCHLD));

        signal_set r(ioc);
        BOOST_TEST(!r.add(SIGWINCH, signal_set::reset_handler));
        BOOST_TEST(!r.remove(SIGWINCH));
    }

    void testAddWithFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with restart flag
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(!result);
    }

    void testAddWithMultipleFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with combined flags
        auto result = s.add(SIGINT, signal_set::restart | signal_set::no_defer);
        BOOST_TEST(!result);
    }

    void testAddSameSignalSameFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal twice with same flags (should be no-op)
        BOOST_TEST(!s.add(SIGINT, signal_set::restart));
        BOOST_TEST(!s.add(SIGINT, signal_set::restart));
    }

    void testAddSameSignalDifferentFlags()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with one flag, then try to add with different flag
        BOOST_TEST(!s.add(SIGINT, signal_set::restart));
        auto result = s.add(SIGINT, signal_set::no_defer);
        BOOST_TEST(!!result); // Should fail due to flag mismatch
    }

    void testAddSameSignalWithDontCare()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with specific flags, then add with dont_care
        BOOST_TEST(!s.add(SIGINT, signal_set::restart));
        auto result = s.add(SIGINT, signal_set::dont_care);
        BOOST_TEST(!result); // Should succeed with dont_care
    }

    void testAddSameSignalDontCareFirst()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with dont_care, then add with specific flags
        BOOST_TEST(!s.add(SIGINT, signal_set::dont_care));
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(!result); // Should succeed
    }

    void testMultipleSetsCompatibleFlags()
    {
        io_context ioc(Backend);
        signal_set s1(ioc);
        signal_set s2(ioc);

        // Both sets add same signal with same flags
        BOOST_TEST(!s1.add(SIGINT, signal_set::restart));
        BOOST_TEST(!s2.add(SIGINT, signal_set::restart));
    }

    void testMultipleSetsIncompatibleFlags()
    {
        io_context ioc(Backend);
        signal_set s1(ioc);
        signal_set s2(ioc);

        // First set adds with one flag
        BOOST_TEST(!s1.add(SIGINT, signal_set::restart));
        // Second set tries to add with different flag
        auto result = s2.add(SIGINT, signal_set::no_defer);
        BOOST_TEST(result == std::errc::invalid_argument);
    }

    void testMultipleSetsWithDontCare()
    {
        io_context ioc(Backend);
        signal_set s1(ioc);
        signal_set s2(ioc);

        // First set adds with specific flags
        BOOST_TEST(!s1.add(SIGINT, signal_set::restart));
        // Second set adds with dont_care
        BOOST_TEST(!s2.add(SIGINT, signal_set::dont_care));
    }

    void testWaitWithFlagsWorks()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Add signal with restart flag and verify wait still works
        BOOST_TEST(!s.add(SIGINT, signal_set::restart));

        bool completed      = false;
        int received_signal = 0;

        auto wait_task = [](signal_set& s_ref, int& sig_out,
                            bool& done_out) -> capy::task<> {
            [[maybe_unused]] auto [ec, signum] = co_await s_ref.wait();
            sig_out                            = signum;
            done_out                           = true;
        };
        capy::run_async(ioc.get_executor())(
            wait_task(s, received_signal, completed));

        auto raise_task = []() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            std::raise(SIGINT);
        };
        capy::run_async(ioc.get_executor())(raise_task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST_EQ(received_signal, SIGINT);
    }

#else // !BOOST_COROSIO_POSIX
    // Signal flags tests (Windows only)

    void testFlagsNotSupportedOnWindows()
    {
        io_context ioc(Backend);
        signal_set s(ioc);

        // Windows returns operation_not_supported for actual flags
        auto result = s.add(SIGINT, signal_set::restart);
        BOOST_TEST(!!result);
        BOOST_TEST(result == std::errc::operation_not_supported);
    }

#endif // BOOST_COROSIO_POSIX

    void run()
    {
        // Construction and move semantics
        testConstruction();
        testConstructWithOneSignal();
        testConstructWithTwoSignals();
        testConstructWithThreeSignals();
        testConstructFromExecutor();
        testConstructFromExecutorWithSignals();
        testMoveConstruct();
        testMoveAssign();
        testMoveAssignCrossContext();

        // Add/remove/clear tests
        testAdd();
        testAddDuplicate();
        testAddInvalidSignal();
        testAddInvalidLargeSignal();
        testRemoveInvalidSignal();
        testRemove();
        testRemoveNotPresent();
        testClear();
        testClearEmpty();

        // Async wait tests
        testWaitWithSignal();
        testWaitWithDifferentSignal();

        // Cancellation tests
        testCancel();
        testCancelBeforeWait();
        testCancelNoWaiters();
        testCancelMultipleTimes();
        testWaitWithPreStoppedToken();
        testMidWaitStopCancellation();
        testStopAfterSignalDelivered();
        testWaitAgainAfterStopCancel();
        testNormalDeliveryWithLiveToken();
        testDestroyWithArmedStopToken();
        testConcurrentStopRequestRace();
        testShutdownWithPendingSignalSet();

        // Multiple signal set tests
        testMultipleSignalSetsOnSameSignal();
        testSignalSetWithMultipleSignals();

        // Queued signal tests
        testQueuedSignal();
        testRemoveOneOfTwoSignals();
        testTwoSetsSameSignalRemoveInBothOrders();
        testSignalDeliveredBeforeWait();
        testTwoServicesDestroyInBothOrders();

        // Registration list surgery

        // Sequential wait tests
        testSequentialWaits();

        // Async-signal-safety / self-pipe regression tests
        testManySequentialSignalCycles();
        testInterleavedQueuedAndWaitedSignals();

        // io_result tests
        testIoResultSuccess();
        testIoResultCanceled();
        testIoResultStructuredBinding();

        // Signal flags tests (cross-platform)
        testFlagsBitwiseOperations();
        testAddWithNoneFlags();
        testAddWithDontCareFlags();

#if BOOST_COROSIO_POSIX
        // Signal flags tests (POSIX only)
        testAddWithFlags();
        testAddWithChildAndResetFlags();
        testAddWithMultipleFlags();
        testAddSameSignalSameFlags();
        testAddSameSignalDifferentFlags();
        testAddSameSignalWithDontCare();
        testAddSameSignalDontCareFirst();
        testMultipleSetsCompatibleFlags();
        testMultipleSetsIncompatibleFlags();
        testMultipleSetsWithDontCare();
        testWaitWithFlagsWorks();
#else
        // Signal flags tests (Windows only)
        testFlagsNotSupportedOnWindows();
#endif

#if !COROSIO_TEST_HAS_ASAN
        // Abandon parked coroutine frames by design; see context.hpp.
        testShutdownReleasesRegistration();
        testShutdownKeepsOtherContextRegistered();
#endif
    }
};

COROSIO_BACKEND_TESTS(signal_set_test, "boost.corosio.signal_set")

} // namespace boost::corosio
