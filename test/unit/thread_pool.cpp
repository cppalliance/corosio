//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/detail/thread_pool.hpp>

#include <boost/corosio/io_context.hpp>

#include <atomic>
#include <thread>

#include "test_suite.hpp"

namespace boost::corosio {

struct test_work : detail::pool_work_item
{
    std::atomic<int>* counter = nullptr;

    static void execute(detail::pool_work_item* w) noexcept
    {
        static_cast<test_work*>(w)->counter->fetch_add(1);
    }
};

struct thread_pool_test
{
    void testDrainOnShutdown()
    {
        io_context ioc;
        auto& pool = ioc.use_service<detail::thread_pool>();

        std::atomic<int> counter{0};

        // Post several tasks before any can run
        constexpr int n = 10;
        test_work items[n];
        for (int i = 0; i < n; ++i)
        {
            items[i].counter = &counter;
            items[i].func_   = &test_work::execute;
            BOOST_TEST(pool.post(&items[i]));
        }

        // Shutdown should drain all queued tasks
        pool.shutdown();

        BOOST_TEST(counter.load() == n);
    }

    void testShutdownWithNoWork()
    {
        io_context ioc;
        auto& pool = ioc.use_service<detail::thread_pool>();

        struct flag_work : detail::pool_work_item
        {
            std::atomic<bool>* flag = nullptr;

            static void execute(detail::pool_work_item* p) noexcept
            {
                static_cast<flag_work*>(p)->flag->store(true);
            }
        };

        std::atomic<bool> ran{false};
        flag_work fw;
        fw.flag  = &ran;
        fw.func_ = &flag_work::execute;
        pool.post(&fw);

        // Give it a moment to process
        while (!ran.load())
            std::this_thread::yield();

        // Shutdown with empty queue should not hang
        pool.shutdown();
    }

    void testPostAfterShutdown()
    {
        io_context ioc;
        auto& pool = ioc.use_service<detail::thread_pool>();

        pool.shutdown();

        // post() must return false after shutdown
        test_work tw;
        std::atomic<int> counter{0};
        tw.counter = &counter;
        tw.func_   = &test_work::execute;
        BOOST_TEST(!pool.post(&tw));
        BOOST_TEST(counter.load() == 0);

        // Second shutdown must not hang
        pool.shutdown();
    }

    void testZeroThreads()
    {
        io_context ioc;

        // Creating a pool with 0 threads must throw
        BOOST_TEST_THROWS(
            detail::thread_pool(ioc, 0),
            std::logic_error);
    }

    void testMultipleThreads()
    {
        io_context ioc;
        constexpr unsigned num_threads = 4;
        detail::thread_pool pool(ioc, num_threads);

        // Each work item blocks on a shared counter until all
        // num_threads items are running, proving true concurrency.
        struct barrier_work : detail::pool_work_item
        {
            std::atomic<unsigned>* arrived;
            unsigned expected;
            std::atomic<int>* done;

            static void execute(detail::pool_work_item* p) noexcept
            {
                auto* self = static_cast<barrier_work*>(p);
                self->arrived->fetch_add(1);
                // Spin until all threads have arrived
                while (self->arrived->load() < self->expected)
                    std::this_thread::yield();
                self->done->fetch_add(1);
            }
        };

        std::atomic<unsigned> arrived{0};
        std::atomic<int> done{0};
        barrier_work items[num_threads];
        for (unsigned i = 0; i < num_threads; ++i)
        {
            items[i].arrived  = &arrived;
            items[i].expected = num_threads;
            items[i].done     = &done;
            items[i].func_    = &barrier_work::execute;
            pool.post(&items[i]);
        }

        pool.shutdown();

        // All items completed — proves all 4 threads ran concurrently
        BOOST_TEST(done.load() == static_cast<int>(num_threads));
    }

    void run()
    {
        testDrainOnShutdown();
        testShutdownWithNoWork();
        testPostAfterShutdown();
        testZeroThreads();
        testMultipleThreads();
    }
};

TEST_SUITE(thread_pool_test, "boost.corosio.thread_pool");

} // namespace boost::corosio
