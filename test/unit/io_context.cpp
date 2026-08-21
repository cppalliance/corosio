//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/io_context.hpp>

#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Coroutine that increments a counter when resumed
struct counter_coro
{
    struct promise_type
    {
        int* counter_ = nullptr;

        counter_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void return_void()
        {
            if (counter_)
                ++(*counter_);
        }

        void unhandled_exception()
        {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> h;

    operator std::coroutine_handle<>() const
    {
        return h;
    }
};

inline counter_coro
make_coro(int& counter)
{
    auto c                 = []() -> counter_coro { co_return; }();
    c.h.promise().counter_ = &counter;
    return c;
}

// Coroutine that increments an atomic counter when resumed
struct atomic_counter_coro
{
    struct promise_type
    {
        std::atomic<int>* counter_ = nullptr;

        atomic_counter_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void return_void()
        {
            if (counter_)
                counter_->fetch_add(1, std::memory_order_relaxed);
        }

        void unhandled_exception()
        {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> h;

    operator std::coroutine_handle<>() const
    {
        return h;
    }
};

inline atomic_counter_coro
make_atomic_coro(std::atomic<int>& counter)
{
    auto c                 = []() -> atomic_counter_coro { co_return; }();
    c.h.promise().counter_ = &counter;
    return c;
}

// Shared state for the peer-utilization barrier (see testHintOneWakesPeers).
struct gate_state
{
    std::mutex              mtx;
    std::condition_variable cv;
    int                     active     = 0;  // gates currently in the barrier
    int                     peak       = 0;  // max concurrent gates observed
    int                     release_at = 0;  // release once this many overlap
};

// Coroutine that, when resumed, blocks until `release_at` gates are running
// concurrently (or a deadline elapses). Observing peak >= 2 requires two
// gates to run on distinct run threads at once, i.e. a parked peer must have
// been woken. Note we release at 2 rather than the gate count: one run thread
// is always occupied servicing the reactor poll, so full N-way overlap is not
// expected.
struct gate_coro
{
    struct promise_type
    {
        gate_state* st = nullptr;

        gate_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }

        void return_void()
        {
            std::unique_lock<std::mutex> lk(st->mtx);
            int cur = ++st->active;
            if (cur > st->peak)
                st->peak = cur;
            if (st->active >= st->release_at)
                st->cv.notify_all();
            else
                st->cv.wait_for(lk, std::chrono::seconds(2),
                    [&] { return st->active >= st->release_at; });
            --st->active;
        }

        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> h;

    operator std::coroutine_handle<>() const { return h; }
};

inline gate_coro
make_gate(gate_state& st)
{
    auto c            = []() -> gate_coro { co_return; }();
    c.h.promise().st  = &st;
    return c;
}

// Coroutine whose promise destructor increments a counter.
// Both initial_suspend and final_suspend return suspend_always so the
// frame is only freed by an explicit .destroy() call.
struct destroy_counter_coro
{
    struct promise_type
    {
        int* counter_ = nullptr;

        destroy_counter_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_always final_suspend() noexcept
        {
            return {};
        }
        void return_void() {}
        void unhandled_exception()
        {
            std::terminate();
        }

        ~promise_type()
        {
            if (counter_)
                ++(*counter_);
        }
    };

    std::coroutine_handle<promise_type> h;

    operator std::coroutine_handle<>() const
    {
        return h;
    }
};

inline destroy_counter_coro
make_destroy_coro(int& counter)
{
    auto c                 = []() -> destroy_counter_coro { co_return; }();
    c.h.promise().counter_ = &counter;
    return c;
}

// Coroutine that checks running_in_this_thread when resumed
struct check_coro
{
    struct promise_type
    {
        bool* result_                  = nullptr;
        io_context::executor_type* ex_ = nullptr;

        check_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_never final_suspend() noexcept
        {
            return {};
        }

        void return_void()
        {
            if (result_ && ex_)
                *result_ = ex_->running_in_this_thread();
        }

        void unhandled_exception()
        {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> h;

    operator std::coroutine_handle<>() const
    {
        return h;
    }
};

inline check_coro
make_check_coro(bool& result, io_context::executor_type& ex)
{
    auto c                = []() -> check_coro { co_return; }();
    c.h.promise().result_ = &result;
    c.h.promise().ex_     = &ex;
    return c;
}

// Helper: post a bare coroutine handle (heap-allocating path).
// Test-only: uses the heap-allocating overload since the handle has
// no enclosing capy::continuation.
inline void
post_coro(io_context::executor_type& ex, std::coroutine_handle<> h)
{
    ex.post(h);
}

inline capy::task<capy::io_result<>>
set_event_task(capy::async_event& evt)
{
    evt.set();
    co_return capy::io_result<>{std::error_code{}};
}

inline capy::task<void>
when_all_set_event_main(bool& finished)
{
    capy::async_event evt;
    auto [ec, a, b] = co_await capy::when_all(evt.wait(), set_event_task(evt));
    (void)a;
    (void)b;
    BOOST_TEST(!ec);
    finished = true;
}

template<auto Backend>
struct io_context_test
{
    void testConstruction()
    {
        // Default construction
        {
            io_context ioc;
            BOOST_TEST(!ioc.stopped());
        }

        // Construction with concurrency hint
        {
            io_context ioc(1);
            BOOST_TEST(!ioc.stopped());
        }
    }

    void testConstructionWithOptions()
    {
        // Tune reactor budgets so the option-applying constructor path
        // exercises non-default values.
        io_context_options opts;
        opts.max_events_per_poll   = 256;
        opts.inline_budget_initial = 4;
        opts.inline_budget_max     = 32;
        opts.unassisted_budget     = 8;

        io_context ioc(opts, 2);
        BOOST_TEST(!ioc.stopped());

        // Single-arg constructor with options + default concurrency
        io_context ioc2(opts);
        BOOST_TEST(!ioc2.stopped());
    }

    void testConstructionWithThreadPoolSize()
    {
        io_context_options opts;
        opts.thread_pool_size = 4;
        io_context ioc(Backend, opts, 2);
        BOOST_TEST(!ioc.stopped());
    }

    void testConstructionSingleThreaded()
    {
        // Lockless mode is enabled solely by opts.locking; the concurrency
        // hint is unrelated to the safety contract.
        io_context_options opts;
        opts.locking = locking_mode::unsafe;
        io_context ioc(Backend, opts, 1);
        BOOST_TEST(!ioc.stopped());

        int counter = 0;
        auto ex     = ioc.get_executor();
        post_coro(ex, make_coro(counter));
        std::size_t n = ioc.run();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    // A lockless tier normalizes the effective concurrency hint to 1 for
    // downstream tuning (reactor budget heuristic, IOCP concurrency),
    // regardless of the hint the caller passed; the safe tier passes it
    // through unchanged. (Independent of Backend; runs per backend harmlessly.)
    void testEffectiveConcurrencyHint()
    {
        io_context_options opts;

        opts.locking = locking_mode::safe;
        BOOST_TEST(detail::effective_concurrency_hint(opts, 8) == 8u);
        BOOST_TEST(detail::effective_concurrency_hint(opts, 1) == 1u);

        opts.locking = locking_mode::unsafe_io;
        BOOST_TEST(detail::effective_concurrency_hint(opts, 8) == 1u);

        opts.locking = locking_mode::unsafe;
        BOOST_TEST(detail::effective_concurrency_hint(opts, 8) == 1u);
    }

    void testConstructionUnsafeIo()
    {
        // The unsafe_io tier elides only per-descriptor I/O locks; the
        // scheduler still runs work normally on its single thread.
        io_context_options opts;
        opts.locking = locking_mode::unsafe_io;
        io_context ioc(Backend, opts, 1);
        BOOST_TEST(!ioc.stopped());

        int counter = 0;
        auto ex     = ioc.get_executor();
        post_coro(ex, make_coro(counter));
        std::size_t n = ioc.run();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void testGetExecutor()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Executor should be valid
        io_context::executor_type ex2 = ioc.get_executor();
        BOOST_TEST(ex == ex2);
    }

    void testRun()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // Post some work
        post_coro(ex, make_coro(counter));
        post_coro(ex, make_coro(counter));
        post_coro(ex, make_coro(counter));

        // Run should execute all work
        std::size_t n = ioc.run();
        BOOST_TEST(n == 3);
        BOOST_TEST(counter == 3);
    }

    void testRunOne()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        post_coro(ex, make_coro(counter));
        post_coro(ex, make_coro(counter));

        // run_one should execute exactly one
        std::size_t n = ioc.run_one();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);

        // run_one again
        n = ioc.run_one();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 2);

        // No more work - would block, so use poll_one instead
    }

    void testPoll()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // Poll with no work should return 0 and stop the context
        std::size_t n = ioc.poll();
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Add work
        post_coro(ex, make_coro(counter));
        post_coro(ex, make_coro(counter));

        // Must restart after stop before poll will process handlers
        ioc.restart();

        // Poll should execute all ready work
        n = ioc.poll();
        BOOST_TEST(n == 2);
        BOOST_TEST(counter == 2);
    }

    void testPollOne()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // poll_one with no work should return 0 and stop the context
        std::size_t n = ioc.poll_one();
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        post_coro(ex, make_coro(counter));
        post_coro(ex, make_coro(counter));

        // Must restart after stop before poll_one will process handlers
        ioc.restart();

        // poll_one should execute exactly one
        n = ioc.poll_one();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);

        n = ioc.poll_one();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 2);

        // No more work - stops again
        n = ioc.poll_one();
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());
    }

    void testStopAndRestart()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        BOOST_TEST(!ioc.stopped());

        // Stop the context
        ioc.stop();
        BOOST_TEST(ioc.stopped());

        // Post work after stop
        post_coro(ex, make_coro(counter));

        // Run should return immediately when stopped
        std::size_t n = ioc.run();
        BOOST_TEST(n == 0);
        BOOST_TEST(counter == 0);

        // Restart
        ioc.restart();
        BOOST_TEST(!ioc.stopped());

        // Now run should work
        n = ioc.run();
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void testRunOneFor()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // run_one_for with no work - returns immediately and stops context
        std::size_t n = ioc.run_one_for(std::chrono::milliseconds(10));
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Must restart before next use
        ioc.restart();

        // With work posted
        post_coro(ex, make_coro(counter));

        n = ioc.run_one_for(std::chrono::milliseconds(100));
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void testRunOneUntil()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // run_one_until with no work - returns immediately and stops context
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        std::size_t n = ioc.run_one_until(deadline);
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Must restart before next use
        ioc.restart();

        // Post work and run_one_until
        post_coro(ex, make_coro(counter));

        deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        n = ioc.run_one_until(deadline);
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void testRunOneUntilExpiredDeadlineStops()
    {
        // An already-elapsed deadline with no work must still stop the
        // context: run_one_until has to call wait_one at least once. This
        // is the deterministic form of a valgrind/CI flake where the thread
        // is preempted past the deadline before the first loop check, so
        // wait_one (which holds the "no work -> stop" logic) was skipped.
        io_context ioc(Backend);

        auto past =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        std::size_t n = ioc.run_one_until(past);
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // run_for with an already-elapsed deadline (run_until -> run_one_until)
        ioc.restart();
        n = ioc.run_for(std::chrono::milliseconds(-1));
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());
    }

    void testRunFor()
    {
        io_context ioc(Backend);
        auto ex     = ioc.get_executor();
        int counter = 0;

        // run_for with no work - returns immediately and stops context
        auto start    = std::chrono::steady_clock::now();
        std::size_t n = ioc.run_for(std::chrono::milliseconds(20));
        auto elapsed  = std::chrono::steady_clock::now() - start;

        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                      .count();
        BOOST_TEST(ms < 15); // Should return immediately when no work

        // Must restart before next use
        ioc.restart();

        // run_for with work
        post_coro(ex, make_coro(counter));
        n = ioc.run_for(std::chrono::milliseconds(100));
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void testRunForWithOutstandingWork()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Simulate persistent outstanding work (like a listening acceptor)
        ex.on_work_started();

        auto start    = std::chrono::steady_clock::now();
        std::size_t n = ioc.run_for(std::chrono::milliseconds(200));
        auto elapsed  = std::chrono::steady_clock::now() - start;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                      .count();

        // Must return after ~200ms, not block forever
        BOOST_TEST(n == 0);
        BOOST_TEST(ms >= 150);
        BOOST_TEST(ms < 1000);

        ex.on_work_finished();
    }

    void testRunOneForWithOutstandingWork()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        ex.on_work_started();

        auto start    = std::chrono::steady_clock::now();
        std::size_t n = ioc.run_one_for(std::chrono::milliseconds(200));
        auto elapsed  = std::chrono::steady_clock::now() - start;

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                      .count();

        BOOST_TEST(n == 0);
        BOOST_TEST(ms >= 150);
        BOOST_TEST(ms < 1000);

        ex.on_work_finished();
    }

    void testExecutorRunningInThisThread()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Not running yet - should return false
        BOOST_TEST(ex.running_in_this_thread() == false);

        // Post work that checks running_in_this_thread
        bool during = false;
        post_coro(ex, make_check_coro(during, ex));
        ioc.run();

        BOOST_TEST(during == true);
    }

    void testMultithreaded()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};
        constexpr int num_threads         = 4;
        constexpr int handlers_per_thread = 100;
        constexpr int total_handlers      = num_threads * handlers_per_thread;

        // Post handlers from multiple threads concurrently
        std::vector<std::thread> posters;
        posters.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
        {
            posters.emplace_back([&ex, &counter]() {
                for (int i = 0; i < handlers_per_thread; ++i)
                    post_coro(ex, make_atomic_coro(counter));
            });
        }

        // Wait for all posters to finish
        for (auto& t : posters)
            t.join();

        // Run with multiple threads
        std::vector<std::thread> runners;
        runners.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
            runners.emplace_back([&ioc]() { ioc.run(); });

        // Wait for all runners to complete
        for (auto& t : runners)
            t.join();

        BOOST_TEST(counter.load() == total_handlers);
    }

    void testHintOneIsThreadSafe()
    {
        // Regression for #310: a plain concurrency_hint of 1 must NOT
        // enable lockless mode. Cross-thread post() into a hint==1
        // context must remain thread-safe (TSan-clean); lockless mode is
        // reachable only via io_context_options::locking.
        io_context ioc(Backend, 1);
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};
        constexpr int num_threads         = 4;
        constexpr int handlers_per_thread = 100;
        constexpr int total_handlers      = num_threads * handlers_per_thread;

        // Post handlers from multiple threads concurrently.
        std::vector<std::thread> posters;
        posters.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
        {
            posters.emplace_back([&ex, &counter]() {
                for (int i = 0; i < handlers_per_thread; ++i)
                    post_coro(ex, make_atomic_coro(counter));
            });
        }
        for (auto& t : posters)
            t.join();

        // Run with multiple threads.
        std::vector<std::thread> runners;
        runners.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
            runners.emplace_back([&ioc]() { ioc.run(); });
        for (auto& t : runners)
            t.join();

        BOOST_TEST(counter.load() == total_handlers);
    }

    void testHintOneWakesPeers()
    {
        // Regression: at concurrency_hint == 1 in the default (safe) tier,
        // multiple run() threads must still be utilized. Internally-generated
        // work has to wake parked peers — the one_thread wake-elision must NOT
        // be engaged by the hint (only by a lockless tier). Each gate holds
        // until two gates overlap, so peak >= 2 is reachable only if a parked
        // peer is woken; if the hint wrongly elides peer wakeups the gates run
        // sequentially (each times out) and peak stays 1.
        constexpr int num_gates   = 4;
        constexpr int num_threads = 4;
        io_context ioc(Backend, 1);

        gate_state st;
        st.release_at = 2;

        // A leader runs on one thread, lets the other run() threads park,
        // then generates the gates from inside the loop (internal work).
        auto leader = [](io_context::executor_type ex,
                         gate_state& state, int gates) -> capy::task<> {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (int i = 0; i < gates; ++i)
                ex.post(make_gate(state));
            co_return;
        };
        capy::run_async(ioc.get_executor())(
            leader(ioc.get_executor(), st, num_gates));

        std::vector<std::thread> runners;
        runners.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t)
            runners.emplace_back([&ioc]() { ioc.run(); });
        for (auto& t : runners)
            t.join();

        BOOST_TEST(st.peak >= 2);
    }

    void testMultithreadedStress()
    {
        // Stress test: multiple iterations of post-then-run with multiple threads
        constexpr int iterations             = 10;
        constexpr int num_threads            = 4;
        constexpr int handlers_per_iteration = 100;

        for (int iter = 0; iter < iterations; ++iter)
        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();
            std::atomic<int> counter{0};

            // Post all handlers first
            for (int i = 0; i < handlers_per_iteration; ++i)
                post_coro(ex, make_atomic_coro(counter));

            // Run with multiple threads
            std::vector<std::thread> runners;
            runners.reserve(num_threads);
            for (int t = 0; t < num_threads; ++t)
                runners.emplace_back([&ioc]() { ioc.run(); });

            for (auto& t : runners)
                t.join();

            auto count = counter.load();
            if (count != handlers_per_iteration)
            {
                std::ostringstream ss;
                ss << "iteration " << iter << ": counter=" << count
                   << ", expected=" << handlers_per_iteration;
                BOOST_ERROR(ss.str().c_str());
            }
        }
    }

    void testWhenAllSetEvent()
    {
        io_context ctx;
        bool finished = false;

        capy::run_async(ctx.get_executor())(when_all_set_event_main(finished));
        ctx.run();

        BOOST_TEST(finished);
    }

    void testShutdownDestroysPostedCoroutineFrames()
    {
        int destroyed = 0;

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            post_coro(ex, make_destroy_coro(destroyed));
            post_coro(ex, make_destroy_coro(destroyed));
            post_coro(ex, make_destroy_coro(destroyed));

            // io_context destructor triggers shutdown
        }

        BOOST_TEST_EQ(destroyed, 3);
    }

    // Exercises scheduler shutdown drain on posted continuations.
    void testContinuationOpDestroyOnShutdown()
    {
        int destroyed = 0;

        // Continuations live outside the io_context scope; the scheduler
        // links them through their own reserved slot and destroys their
        // coroutine frames on shutdown.
        capy::continuation cont1;
        capy::continuation cont2;
        cont1.h = make_destroy_coro(destroyed);
        cont2.h = make_destroy_coro(destroyed);

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            ex.post(cont1);
            ex.post(cont2);

            // io_context destructor drains the ready queue and calls
            // h.destroy() on each queued continuation's handle.
        }

        BOOST_TEST_EQ(destroyed, 2);
    }

    // Exercises the `rel_time > 1s` clamp branch in run_one_until.
    // With no work and a deadline >1s in the future, the inner loop
    // iterates with rel_time clamped to 1s before returning 0.
    void testRunOneUntilLongDeadlineNoWork()
    {
        io_context ioc(Backend);

        // Deadline >1s but tiny outstanding work so wait_one is not
        // entered: scheduler is empty, wait_one immediately stops and
        // returns 0. The outer run_one_until loop still enters with
        // rel_time > 1s, hitting the clamp branch.
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        std::size_t n = ioc.run_one_until(deadline);
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());
    }

    // MT-mode test that exercises conditionally_enabled_event::wait_for()
    // and cross-thread notify_one(). A work guard keeps run_for inside its
    // wait_for; main posts 8 handlers from a different thread, each of
    // which triggers notify_one. Main polls counter until all handlers
    // have run, then releases the work guard so run_for exits. The
    // assertion depends only on the completed work, not on wall-clock
    // timing.
    void testMultithreadedNotifyAndWaitFor()
    {
        io_context ioc(Backend); // default hint => MT mode
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};

        // Work guard prevents run_for from short-circuiting on an
        // empty queue before main posts any work.
        ex.on_work_started();

        std::thread runner([&]() {
            // 5s ceiling is a safety net only; we release the guard
            // below as soon as work is drained.
            (void)ioc.run_for(std::chrono::seconds(5));
        });

        for (int i = 0; i < 8; ++i)
            post_coro(ex, make_atomic_coro(counter));

        while (counter.load() < 8)
            std::this_thread::yield();

        ex.on_work_finished();
        runner.join();

        BOOST_TEST_EQ(counter.load(), 8);
    }

#if BOOST_COROSIO_POSIX
    void testZeroThreadPoolSizeThrows()
    {
        io_context_options opts;
        opts.thread_pool_size = 0;
        bool threw = false;
        try
        {
            io_context ioc(opts);
        }
        catch (std::invalid_argument const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
    }
#endif

    void run()
    {
#if BOOST_COROSIO_POSIX
        testZeroThreadPoolSizeThrows();
#endif
        testConstruction();
        testConstructionWithOptions();
        testConstructionWithThreadPoolSize();
        testConstructionSingleThreaded();
        testConstructionUnsafeIo();
        testEffectiveConcurrencyHint();
        testGetExecutor();
        testRun();
        testRunOne();
        testPoll();
        testPollOne();
        testStopAndRestart();
        testRunOneFor();
        testRunOneUntil();
        testRunOneUntilExpiredDeadlineStops();
        testRunOneUntilLongDeadlineNoWork();
        testRunFor();
        testRunForWithOutstandingWork();
        testRunOneForWithOutstandingWork();
        testExecutorRunningInThisThread();
        testMultithreaded();
        testHintOneIsThreadSafe();
        testHintOneWakesPeers();
        testMultithreadedStress();
        testMultithreadedNotifyAndWaitFor();
        testWhenAllSetEvent();
        testShutdownDestroysPostedCoroutineFrames();
        testContinuationOpDestroyOnShutdown();
    }
};

COROSIO_BACKEND_TESTS(io_context_test, "boost.corosio.io_context")

// Backend-parameterized tests for shutdown paths that differ per backend
template<auto Backend>
struct io_context_shutdown_test
{
    void testShutdownDestroysPostedCoroutineFrames()
    {
        int destroyed = 0;

        {
            io_context ioc(Backend);
            auto ex = ioc.get_executor();

            post_coro(ex, make_destroy_coro(destroyed));
            post_coro(ex, make_destroy_coro(destroyed));
            post_coro(ex, make_destroy_coro(destroyed));
        }

        BOOST_TEST_EQ(destroyed, 3);
    }

    void testConstructionWithBackendAndOptions()
    {
        // Exercises the templated io_context(Backend, options, hint)
        // constructor that runs apply_options_pre_/_post_.
        io_context_options opts;
        opts.max_events_per_poll   = 64;
        opts.inline_budget_initial = 4;
        opts.inline_budget_max     = 16;
        opts.unassisted_budget     = 4;

        io_context ioc(Backend, opts, 2);
        BOOST_TEST(!ioc.stopped());
    }

    void run()
    {
        testShutdownDestroysPostedCoroutineFrames();
        testConstructionWithBackendAndOptions();
    }
};

COROSIO_BACKEND_TESTS(
    io_context_shutdown_test, "boost.corosio.io_context_shutdown")

} // namespace boost::corosio
