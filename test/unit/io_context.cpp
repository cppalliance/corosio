//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/io_context.hpp>

#include <boost/capy/concept/executor.hpp>
#include <boost/capy/coro.hpp>

#include <atomic>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

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

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void()
        {
            if (counter_)
                ++(*counter_);
        }

        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> h;

    operator capy::coro() const { return h; }
};

inline counter_coro make_coro(int& counter)
{
    auto c = []() -> counter_coro { co_return; }();
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

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void()
        {
            if (counter_)
                counter_->fetch_add(1, std::memory_order_relaxed);
        }

        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> h;

    operator capy::coro() const { return h; }
};

inline atomic_counter_coro make_atomic_coro(std::atomic<int>& counter)
{
    auto c = []() -> atomic_counter_coro { co_return; }();
    c.h.promise().counter_ = &counter;
    return c;
}

// Coroutine that checks running_in_this_thread when resumed
struct check_coro
{
    struct promise_type
    {
        bool* result_ = nullptr;
        io_context::executor_type* ex_ = nullptr;

        check_coro get_return_object()
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void()
        {
            if (result_ && ex_)
                *result_ = ex_->running_in_this_thread();
        }

        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> h;

    operator capy::coro() const { return h; }
};

inline check_coro make_check_coro(bool& result, io_context::executor_type& ex)
{
    auto c = []() -> check_coro { co_return; }();
    c.h.promise().result_ = &result;
    c.h.promise().ex_ = &ex;
    return c;
}

struct io_context_test
{
    void
    testConstruction()
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

    void
    testGetExecutor()
    {
        io_context ioc;
        auto ex = ioc.get_executor();

        // Executor should be valid
        io_context::executor_type ex2 = ioc.get_executor();
        BOOST_TEST(ex == ex2);
    }

    void
    testRun()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // Post some work
        ex.post(make_coro(counter));
        ex.post(make_coro(counter));
        ex.post(make_coro(counter));

        // Run should execute all work
        std::size_t n = ioc.run();
        BOOST_TEST(n == 3);
        BOOST_TEST(counter == 3);
    }

    void
    testRunOne()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        ex.post(make_coro(counter));
        ex.post(make_coro(counter));

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

    void
    testPoll()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // Poll with no work should return 0 and stop the context
        std::size_t n = ioc.poll();
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Add work
        ex.post(make_coro(counter));
        ex.post(make_coro(counter));

        // Must restart after stop before poll will process handlers
        ioc.restart();

        // Poll should execute all ready work
        n = ioc.poll();
        BOOST_TEST(n == 2);
        BOOST_TEST(counter == 2);
    }

    void
    testPollOne()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // poll_one with no work should return 0 and stop the context
        std::size_t n = ioc.poll_one();
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        ex.post(make_coro(counter));
        ex.post(make_coro(counter));

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

    void
    testStopAndRestart()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        BOOST_TEST(!ioc.stopped());

        // Stop the context
        ioc.stop();
        BOOST_TEST(ioc.stopped());

        // Post work after stop
        ex.post(make_coro(counter));

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

    void
    testRunOneFor()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // run_one_for with no work - returns immediately and stops context
        std::size_t n = ioc.run_one_for(std::chrono::milliseconds(10));
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Must restart before next use
        ioc.restart();

        // With work posted
        ex.post(make_coro(counter));

        n = ioc.run_one_for(std::chrono::milliseconds(100));
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void
    testRunOneUntil()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // run_one_until with no work - returns immediately and stops context
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        std::size_t n = ioc.run_one_until(deadline);
        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());

        // Must restart before next use
        ioc.restart();

        // Post work and run_one_until
        ex.post(make_coro(counter));

        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        n = ioc.run_one_until(deadline);
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void
    testRunFor()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;

        // run_for with no work - returns immediately and stops context
        auto start = std::chrono::steady_clock::now();
        std::size_t n = ioc.run_for(std::chrono::milliseconds(20));
        auto elapsed = std::chrono::steady_clock::now() - start;

        BOOST_TEST(n == 0);
        BOOST_TEST(ioc.stopped());
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        BOOST_TEST(ms < 15); // Should return immediately when no work

        // Must restart before next use
        ioc.restart();

        // run_for with work
        ex.post(make_coro(counter));
        n = ioc.run_for(std::chrono::milliseconds(100));
        BOOST_TEST(n == 1);
        BOOST_TEST(counter == 1);
    }

    void
    testExecutorRunningInThisThread()
    {
        io_context ioc;
        auto ex = ioc.get_executor();

        // Not running yet - should return false
        BOOST_TEST(ex.running_in_this_thread() == false);

        // Post work that checks running_in_this_thread
        bool during = false;
        ex.post(make_check_coro(during, ex));
        ioc.run();

        BOOST_TEST(during == true);
    }

    void
    testMultithreaded()
    {
        io_context ioc;
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};
        constexpr int num_threads = 4;
        constexpr int handlers_per_thread = 100;
        constexpr int total_handlers = num_threads * handlers_per_thread;

        // Post handlers from multiple threads concurrently
        std::vector<std::thread> posters;
        for (int t = 0; t < num_threads; ++t)
        {
            posters.emplace_back([&ex, &counter]() {
                for (int i = 0; i < handlers_per_thread; ++i)
                    ex.post(make_atomic_coro(counter));
            });
        }

        // Wait for all posters to finish
        for (auto& t : posters)
            t.join();

        // Run with multiple threads
        std::vector<std::thread> runners;
        for (int t = 0; t < num_threads; ++t)
            runners.emplace_back([&ioc]() { ioc.run(); });

        // Wait for all runners to complete
        for (auto& t : runners)
            t.join();

        BOOST_TEST(counter.load() == total_handlers);
    }

    void
    testMultithreadedStress()
    {
        // Stress test: multiple iterations of post-then-run with multiple threads
        constexpr int iterations = 10;
        constexpr int num_threads = 4;
        constexpr int handlers_per_iteration = 100;

        for (int iter = 0; iter < iterations; ++iter)
        {
            io_context ioc;
            auto ex = ioc.get_executor();
            std::atomic<int> counter{0};

            // Post all handlers first
            for (int i = 0; i < handlers_per_iteration; ++i)
                ex.post(make_atomic_coro(counter));

            // Run with multiple threads
            std::vector<std::thread> runners;
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

    void
    run()
    {
        testConstruction();
        testGetExecutor();
        testRun();
        testRunOne();
        testPoll();
        testPollOne();
        testStopAndRestart();
        testRunOneFor();
        testRunOneUntil();
        testRunFor();
        testExecutorRunningInThisThread();
        testMultithreaded();
        testMultithreadedStress();
    }
};

TEST_SUITE(io_context_test, "boost.corosio.io_context");

} // namespace corosio
} // namespace boost
