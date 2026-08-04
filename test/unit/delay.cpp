//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/delay.hpp>

#include <boost/corosio/timeout.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Test clock driven by a static counter so facade waits complete
// after a deterministic number of re-check iterations with no
// wall-clock dependence.
struct test_clock
{
    using rep        = std::int64_t;
    using period     = std::nano;
    using duration   = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<test_clock>;
    static constexpr bool is_steady = false;

    static inline std::atomic<std::int64_t> now_ns{0};

    static time_point now() noexcept
    {
        return time_point(
            duration(now_ns.load(std::memory_order_relaxed)));
    }
};

// Advances test_clock 1ms per consultation and requests a zero-length
// steady wait, so each re-check lands on the next reactor pass.
struct stepping_traits
{
    static inline std::atomic<int> calls{0};

    static test_clock::duration
    to_wait_duration(test_clock::duration)
    {
        calls.fetch_add(1, std::memory_order_relaxed);
        test_clock::now_ns.fetch_add(
            1'000'000, std::memory_order_relaxed);
        return {};
    }
};

// Requests a wait far longer than any test runs, so cancellation is
// the only way out.
struct hold_traits
{
    static test_clock::duration
    to_wait_duration(test_clock::duration)
    {
        return std::chrono::seconds(10);
    }
};

// Distinct clock type backed by the monotonic clock: routes to the
// facade ( not the steady fast path ) but advances in real time,
// exercising default traits end to end.
struct wall_clock
{
    using rep        = std::chrono::steady_clock::rep;
    using period     = std::chrono::steady_clock::period;
    using duration   = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<wall_clock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept
    {
        return time_point(
            std::chrono::steady_clock::now().time_since_epoch());
    }
};

// Steady time points, of any duration, must keep the zero-iteration
// awaitable; other clocks route to the facade.
static_assert(std::same_as<
    decltype(delay(std::chrono::steady_clock::time_point{})),
    delay_awaitable>);
static_assert(std::same_as<
    decltype(delay(std::chrono::time_point<std::chrono::steady_clock,
        std::chrono::milliseconds>{})),
    delay_awaitable>);
static_assert(std::same_as<
    decltype(delay(test_clock::time_point{})),
    clock_delay_awaitable<test_clock, wait_traits<test_clock>>>);
static_assert(std::same_as<
    decltype(delay<stepping_traits>(test_clock::time_point{})),
    clock_delay_awaitable<test_clock, stepping_traits>>);

template<auto Backend>
struct delay_test
{
    void testDurationCompletes()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(5));
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testTimePointCompletes()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(5);
            auto [ec] = co_await delay(tp);
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testZeroDurationCompletesImmediately()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(0));
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testPastTimePointCompletesImmediately()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = std::chrono::steady_clock::now() -
                std::chrono::seconds(1);
            auto [ec] = co_await delay(tp);
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testCancellation()
    {
        io_context ioc(Backend);
        std::stop_source src;
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::seconds(10));
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        // Let the delay suspend, then cancel
        ioc.run_one();
        src.request_stop();
        ioc.run();
        BOOST_TEST(canceled);
    }

    void testAlreadyStoppedCompletesCanceled()
    {
        io_context ioc(Backend);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::seconds(10));
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testZeroDurationWithStopRequested()
    {
        io_context ioc(Backend);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(0));
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testPastTimePointWithStopRequested()
    {
        io_context ioc(Backend);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto tp = std::chrono::steady_clock::now() -
                std::chrono::seconds(1);
            auto [ec] = co_await delay(tp);
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testSingleThreadedHint()
    {
        // concurrency_hint == 1 enables single-threaded fast paths;
        // delay must behave identically (spec sub-item d).
        io_context ioc(Backend, 1u);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(5));
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testSequentialDelays()
    {
        io_context ioc(Backend);
        int count = 0;

        auto t = [](int& count_out) -> capy::task<> {
            for(int i = 0; i < 3; ++i)
            {
                auto [ec] = co_await delay(std::chrono::milliseconds(1));
                if(!ec)
                    ++count_out;
            }
        };
        capy::run_async(ioc.get_executor())(t(count));

        ioc.run();
        BOOST_TEST_EQ(count, 3);
    }

    // Issue: an executor whose context is not an io_context cannot
    // supply a timer service. await_suspend is normally reached only
    // through a noexcept coroutine-resumption path, where the
    // resulting std::logic_error would terminate rather than throw;
    // calling it directly here (outside that path) exercises the
    // same precondition check catchably.
    void testNonIoContextThrows()
    {
        capy::thread_pool pool(1);
        auto ex = pool.get_executor();
        delay_awaitable da(std::chrono::milliseconds(1));
        capy::io_env env{ex, {}, {}};
        BOOST_TEST_THROWS(
            da.await_suspend(std::noop_coroutine(), &env),
            std::logic_error);
    }

    void testDelayActuallyWaits()
    {
        io_context ioc(Backend);
        std::chrono::steady_clock::duration elapsed{};

        auto t = [](std::chrono::steady_clock::duration& out) -> capy::task<> {
            auto start = std::chrono::steady_clock::now();
            auto [ec]  = co_await delay(std::chrono::milliseconds(50));
            out        = std::chrono::steady_clock::now() - start;
            (void)ec;
        };
        capy::run_async(ioc.get_executor())(t(elapsed));

        ioc.run();
        BOOST_TEST(elapsed >= std::chrono::milliseconds(50));
    }

    void testConcurrentDelaysHeapRemoval()
    {
        // Several concurrent delays at interleaved deadlines; the
        // middle ones are canceled through their own stop tokens,
        // exercising heap middle-removal. All must complete with the
        // expected disposition.
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        std::stop_source s1, s2, s3, s4, s5;
        int ok = 0;
        int canceled = 0;

        // Deadlines interleaved (5,4,3,2,1 ms); cancel the middle
        // three (3,4,5 ms) so removals land in the heap interior.
        auto d = [](int ms, int& ok_out, int& cancel_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(ms));
            if (ec == capy::cond::canceled)
                ++cancel_out;
            else if (!ec)
                ++ok_out;
        };

        capy::run_async(ex, s1.get_token())(d(1, ok, canceled));
        capy::run_async(ex, s2.get_token())(d(2, ok, canceled));
        capy::run_async(ex, s3.get_token())(d(3, ok, canceled));
        capy::run_async(ex, s4.get_token())(d(4, ok, canceled));
        capy::run_async(ex, s5.get_token())(d(5, ok, canceled));

        // Let all five suspend into the heap, then cancel the middle
        // three before any fires.
        ioc.poll();
        s3.request_stop();
        s4.request_stop();
        s5.request_stop();

        ioc.run();
        BOOST_TEST_EQ(canceled, 3);
        BOOST_TEST_EQ(ok, 2);
    }

    void testShutdownWithSuspendedDelay()
    {
        // Destroying the io_context while a delay is suspended must
        // drain the heap waiter and destroy the coroutine frame
        // cleanly (guard runs).
        int destroyed = 0;

        {
            io_context ioc(Backend);

            auto task = [](int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard() { ++c_; }
                };
                guard g{counter};
                auto [ec] = co_await delay(std::chrono::hours(1));
                (void)ec;
            };

            capy::run_async(ioc.get_executor())(task(destroyed));
            ioc.poll();
            // io_context destructs with the delay still suspended;
            // timer_service::shutdown() drains the heap waiter.
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    void testShutdownDrainsHeapWaiters()
    {
        // Several delays at different deadlines populate the heap; all
        // are suspended when the io_context is destroyed.
        int destroyed = 0;

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            auto task = [](int ms, int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard() { ++c_; }
                };
                guard g{counter};
                auto [ec] = co_await delay(std::chrono::hours(ms));
                (void)ec;
            };

            capy::run_async(ex)(task(1, destroyed));
            capy::run_async(ex)(task(2, destroyed));
            capy::run_async(ex)(task(3, destroyed));
            ioc.poll();
        }

        BOOST_TEST_EQ(destroyed, 3);
    }

    void testShutdownDrainsPostedCompletion()
    {
        // A cancelled delay posts its completion op to the scheduler
        // ready queue; the io_context destructs before running it, so
        // the scheduler drain must destroy the suspended frame through
        // the queued op rather than the timer service's heap drain.
        int destroyed = 0;
        std::stop_source src;

        {
            io_context ioc(Backend);

            auto task = [](int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard() { ++c_; }
                };
                guard g{counter};
                auto [ec] = co_await delay(std::chrono::hours(1));
                (void)ec;
            };

            capy::run_async(ioc.get_executor(), src.get_token())(
                task(destroyed));
            ioc.poll();
            src.request_stop();
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    void testNarrowRepDurationClamp()
    {
        // A narrow-rep duration must survive the overflow clamp intact.
        // The old clamp converted nanoseconds::max() into the caller's
        // rep, which wrapped and pushed a 5ms wait out to ~292 years
        // (an effective hang). It must complete promptly instead.
        io_context ioc(Backend);
        bool ok = false;
        std::chrono::steady_clock::duration elapsed{};

        auto t = [](bool& ok_out,
                    std::chrono::steady_clock::duration& out) -> capy::task<> {
            auto start = std::chrono::steady_clock::now();
            auto [ec]  = co_await delay(
                std::chrono::duration<std::int32_t, std::milli>(5));
            out    = std::chrono::steady_clock::now() - start;
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok, elapsed));

        ioc.run();
        BOOST_TEST(ok);
        // A mis-clamp would never complete within this bound.
        BOOST_TEST(elapsed < std::chrono::seconds(5));
    }

    void testNegativeExtremeDurationCompletes()
    {
        // A hugely negative coarse duration is documented (negative)
        // input; the noexcept clamp must not invoke duration_cast UB,
        // and the wait completes immediately as a success.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto [ec] = co_await delay(
                std::chrono::duration<std::int64_t, std::ratio<3600>>::min());
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testFloatingNaNDurationCompletes()
    {
        // A NaN floating-rep duration must not reach duration_cast;
        // the clamp treats it as no wait and completes synchronously.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::duration<double>(
                std::numeric_limits<double>::quiet_NaN()));
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testPositiveExtremeDurationArmsThenCancels()
    {
        // hours::max() must clamp and arm a real timer without overflow
        // UB in the noexcept clamp, then resolve through the stop token
        // rather than waiting out the (effectively infinite) deadline.
        io_context ioc(Backend);
        std::stop_source src;
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto [ec]    = co_await delay((std::chrono::hours::max)());
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run_one();
        src.request_stop();
        ioc.run();
        BOOST_TEST(canceled);
    }

    void testMultiTimerExpiryOrder()
    {
        // Three delays with distinct deadlines must complete in deadline
        // order on a single run thread.
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        std::vector<int> order;

        auto d = [](int ms, int id, std::vector<int>& out) -> capy::task<> {
            auto [ec] = co_await delay(std::chrono::milliseconds(ms));
            (void)ec;
            out.push_back(id);
        };

        // Spawn out of deadline order to prove ordering is by deadline.
        capy::run_async(ex)(d(30, 3, order));
        capy::run_async(ex)(d(10, 1, order));
        capy::run_async(ex)(d(20, 2, order));

        ioc.run();
        BOOST_TEST_EQ(order.size(), 3u);
        if (order.size() == 3)
        {
            BOOST_TEST_EQ(order[0], 1);
            BOOST_TEST_EQ(order[1], 2);
            BOOST_TEST_EQ(order[2], 3);
        }
    }

    void testAbruptStopWithPendingDelays()
    {
        // io_context::stop() mid-run leaves armed delay waiters in the
        // heap; the subsequent destruction must drain them without
        // hanging.
        bool started = false;

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            auto waiter = [](bool& started_out) -> capy::task<> {
                started_out = true;
                auto [ec]   = co_await delay(std::chrono::hours(1));
                (void)ec;
            };
            auto stopper = [](io_context& ctx) -> capy::task<> {
                ctx.stop();
                co_return;
            };

            capy::run_async(ex)(waiter(started));
            capy::run_async(ex)(waiter(started));
            capy::run_async(ex)(waiter(started));
            capy::run_async(ex)(stopper(ioc));

            ioc.run();
            BOOST_TEST(started);
            // io_context destructs with three pending delay waiters.
        }
        BOOST_TEST_PASS();
    }

    void testShutdownReentrantThreeFrames()
    {
        // Three suspended delay()/timeout() frames each own a timer that
        // sits in the heap at teardown. Destroying a frame re-enters
        // destroy_impl (removing a heap entry) while shutdown() iterates
        // the heap, so the drain must tolerate re-entrant removal.
        int destroyed = 0;

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            auto delay_frame = [](int& counter) -> capy::task<> {
                struct guard { int& c_; ~guard() { ++c_; } };
                guard g{counter};
                auto [ec] = co_await delay(std::chrono::hours(1));
                (void)ec;
            };
            auto timeout_frame = [](int& counter) -> capy::task<> {
                struct guard { int& c_; ~guard() { ++c_; } };
                guard g{counter};
                auto [ec] = co_await timeout(
                    delay(std::chrono::hours(1)), std::chrono::hours(1));
                (void)ec;
            };

            capy::run_async(ex)(delay_frame(destroyed));
            capy::run_async(ex)(timeout_frame(destroyed));
            capy::run_async(ex)(delay_frame(destroyed));
            ioc.poll();
        }

        BOOST_TEST_EQ(destroyed, 3);
    }

    void testInitiationStopRace()
    {
        // Stress the initiation-ordering race in wait(): foreign
        // threads request stop on delay waiters while run() threads
        // may fire them, hammering the publication window. Every
        // co_await must complete (success or canceled) and the
        // context must drain (no lost work, no hang).
        constexpr int N = 200;

        for (int iter = 0; iter < 5; ++iter)
        {
            io_context ioc(Backend, 2u); // multi-threaded, not hint 1
            auto ex = ioc.get_executor();

            std::atomic<int> completed{0};
            std::vector<std::stop_source> srcs(N);

            auto task =
                [](std::atomic<int>& done, int ms) -> capy::task<> {
                auto [ec] = co_await delay(std::chrono::milliseconds(ms));
                (void)ec; // success or canceled — both acceptable
                done.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < N; ++i)
                capy::run_async(ex, srcs[i].get_token())(
                    task(completed, i % 3)); // 0-2ms staggered

            std::thread stopper([&] {
                for (int i = 0; i < N; ++i)
                    srcs[i].request_stop();
            });
            std::thread r1([&] { ioc.run(); });
            std::thread r2([&] { ioc.run(); });

            r1.join();
            r2.join();
            stopper.join();

            BOOST_TEST_EQ(completed.load(), N);
        }
    }

    void testClockDeadlineCompletes()
    {
        io_context ioc(Backend);
        test_clock::now_ns.store(0);
        stepping_traits::calls.store(0);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = test_clock::now() + std::chrono::milliseconds(5);
            auto [ec] = co_await delay<stepping_traits>(tp);
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
        // The re-check loop must have run: 5ms of clock at 1ms per
        // consultation is at least four re-arms after the initial one.
        BOOST_TEST(stepping_traits::calls.load() >= 4);
        BOOST_TEST(test_clock::now() >= test_clock::time_point(
            std::chrono::milliseconds(5)));
    }

    void testClockPastDeadlineCompletesImmediately()
    {
        io_context ioc(Backend);
        test_clock::now_ns.store(1'000'000'000);
        stepping_traits::calls.store(0);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = test_clock::now() - std::chrono::seconds(1);
            auto [ec] = co_await delay<stepping_traits>(tp);
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
        // Elapsed deadline resumes inline without consulting traits
        BOOST_TEST_EQ(stepping_traits::calls.load(), 0);
    }

    void testClockDefaultTraitsCompletes()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = wall_clock::now() + std::chrono::milliseconds(5);
            auto [ec] = co_await delay(tp);
            ok_out = !ec && wall_clock::now() >= tp;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testClockCoarseDurationCompletes()
    {
        // A time_point coarser than Clock::duration must convert
        // ( ceil ) and still complete at or after the deadline.
        io_context ioc(Backend);
        test_clock::now_ns.store(0);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            auto tp = std::chrono::time_point<test_clock,
                std::chrono::milliseconds>(std::chrono::milliseconds(3));
            auto [ec] = co_await delay<stepping_traits>(tp);
            ok_out = !ec && test_clock::now() >=
                test_clock::time_point(std::chrono::milliseconds(3));
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testClockCancellation()
    {
        io_context ioc(Backend);
        test_clock::now_ns.store(0);
        std::stop_source src;
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto tp = test_clock::now() + std::chrono::hours(1);
            auto [ec] = co_await delay<hold_traits>(tp);
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        // Let the wait suspend, then cancel
        ioc.run_one();
        src.request_stop();
        ioc.run();
        BOOST_TEST(canceled);
    }

    void testClockAlreadyStoppedCompletesCanceled()
    {
        io_context ioc(Backend);
        test_clock::now_ns.store(0);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto tp = test_clock::now() + std::chrono::hours(1);
            auto [ec] = co_await delay<hold_traits>(tp);
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testClockPastDeadlineWithStopRequested()
    {
        // Stop must win over an elapsed deadline, mirroring the
        // steady overloads' ordering.
        io_context ioc(Backend);
        test_clock::now_ns.store(1'000'000'000);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& canceled_out) -> capy::task<> {
            auto tp = test_clock::now() - std::chrono::seconds(1);
            auto [ec] = co_await delay<hold_traits>(tp);
            canceled_out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testClockShutdownWithSuspendedWait()
    {
        // Destroying the io_context while a facade wait is suspended
        // must drain the waiter and destroy the frame ( guard runs ),
        // like any pending steady wait.
        int destroyed = 0;
        test_clock::now_ns.store(0);

        {
            io_context ioc(Backend);

            auto task = [](int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard() { ++c_; }
                };
                guard g{counter};
                auto tp = test_clock::now() + std::chrono::hours(1);
                auto [ec] = co_await delay<hold_traits>(tp);
                (void)ec;
            };

            capy::run_async(ioc.get_executor())(task(destroyed));
            ioc.poll();
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    void testClockRearmStopRace()
    {
        // Hammer the rearm/cancel interleavings: race_traits spins the
        // facade at reactor rate while a foreign thread requests stop
        // on every waiter. The clock still advances 1us per re-arm, so
        // the test terminates even if every stop were lost. Every
        // co_await must complete and the context must drain.
        struct race_traits
        {
            static test_clock::duration
            to_wait_duration(test_clock::duration)
            {
                test_clock::now_ns.fetch_add(
                    1'000, std::memory_order_relaxed);
                return {};
            }
        };

        constexpr int N = 100;

        for(int iter = 0; iter < 5; ++iter)
        {
            io_context ioc(Backend, 2u); // multi-threaded, not hint 1
            auto ex = ioc.get_executor();
            test_clock::now_ns.store(0);

            std::atomic<int> completed{0};
            std::vector<std::stop_source> srcs(N);

            auto task = [](std::atomic<int>& done) -> capy::task<> {
                auto tp = test_clock::now() +
                    std::chrono::milliseconds(50);
                auto [ec] = co_await delay<race_traits>(tp);
                (void)ec; // success or canceled — both acceptable
                done.fetch_add(1, std::memory_order_relaxed);
            };

            for(int i = 0; i < N; ++i)
                capy::run_async(ex, srcs[i].get_token())(task(completed));

            std::thread stopper([&] {
                for(auto& s : srcs)
                    s.request_stop();
            });
            std::thread r1([&] { ioc.run(); });
            std::thread r2([&] { ioc.run(); });

            r1.join();
            r2.join();
            stopper.join();

            BOOST_TEST_EQ(completed.load(), N);
        }
    }

    void run()
    {
        testDurationCompletes();
        testTimePointCompletes();
        testZeroDurationCompletesImmediately();
        testPastTimePointCompletesImmediately();
        testCancellation();
        testAlreadyStoppedCompletesCanceled();
        testZeroDurationWithStopRequested();
        testPastTimePointWithStopRequested();
        testSingleThreadedHint();
        testSequentialDelays();
        testNonIoContextThrows();
        testDelayActuallyWaits();
        testConcurrentDelaysHeapRemoval();
        testShutdownWithSuspendedDelay();
        testShutdownDrainsHeapWaiters();
        testShutdownDrainsPostedCompletion();
        testNarrowRepDurationClamp();
        testNegativeExtremeDurationCompletes();
        testFloatingNaNDurationCompletes();
        testPositiveExtremeDurationArmsThenCancels();
        testMultiTimerExpiryOrder();
        testAbruptStopWithPendingDelays();
        testShutdownReentrantThreeFrames();
        testInitiationStopRace();
        testClockDeadlineCompletes();
        testClockPastDeadlineCompletesImmediately();
        testClockDefaultTraitsCompletes();
        testClockCoarseDurationCompletes();
        testClockCancellation();
        testClockAlreadyStoppedCompletesCanceled();
        testClockPastDeadlineWithStopRequested();
        testClockShutdownWithSuspendedWait();
        testClockRearmStopRace();
    }
};

COROSIO_BACKEND_TESTS(delay_test, "boost.corosio.delay")

} // namespace boost::corosio
