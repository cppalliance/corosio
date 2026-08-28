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

#if defined(__linux__)
#include <dirent.h>
#include <cstdio>
#include <cstring>
#endif

#include "test_suite.hpp"

namespace boost::corosio {

#if defined(__linux__)

// Count this process's live pool workers by the name each one gives
// itself before it can run anything, so what is counted is the
// spawning and not the timing of it.
inline int
pool_thread_count()
{
    auto* d = ::opendir("/proc/self/task");
    if (!d)
        return -1;
    int n = 0;
    while (auto* e = ::readdir(d))
    {
        if (e->d_name[0] == '.')
            continue;
        // Sized for the longest name the directory entry can carry,
        // so the path is never truncated.
        char path[sizeof("/proc/self/task//comm") + sizeof(e->d_name)];
        std::snprintf(
            path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        auto* f = std::fopen(path, "r");
        if (!f)
            continue;
        char name[32] = {};
        if (std::fgets(name, sizeof(name), f) &&
            std::strncmp(name, "tpool-svc-", 10) == 0)
            ++n;
        std::fclose(f);
    }
    ::closedir(d);
    return n;
}

#endif

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
            BOOST_TEST(!pool.post(&items[i]));
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
        BOOST_TEST(!pool.post(&fw));

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

        // A shutdown pool answers a post with the cancellation its
        // callers report, and starts no thread doing it.
        test_work tw;
        std::atomic<int> counter{0};
        tw.counter = &counter;
        tw.func_   = &test_work::execute;
        BOOST_TEST(pool.post(&tw) == capy::error::canceled);
        BOOST_TEST(pool.worker_count() == 0);
        BOOST_TEST(counter.load() == 0);

        // Second shutdown must not hang
        pool.shutdown();
    }

    void testZeroThreads()
    {
        io_context ioc;

        // Creating a pool with 0 threads must throw
        BOOST_TEST_THROWS(detail::thread_pool(ioc, 0), std::logic_error);
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
            std::atomic<bool>* give_up;

            static void execute(detail::pool_work_item* p) noexcept
            {
                auto* self = static_cast<barrier_work*>(p);
                self->arrived->fetch_add(1);
                // Spin until all threads have arrived, or until the
                // test says the arrival it is waiting for is one the
                // pool never started a worker for.
                while (self->arrived->load() < self->expected &&
                       !self->give_up->load())
                    std::this_thread::yield();
                self->done->fetch_add(1);
            }
        };

        std::atomic<unsigned> arrived{0};
        std::atomic<int> done{0};
        std::atomic<bool> give_up{false};
        barrier_work items[num_threads];
        for (unsigned i = 0; i < num_threads; ++i)
        {
            items[i].arrived  = &arrived;
            items[i].expected = num_threads;
            items[i].done     = &done;
            items[i].give_up  = &give_up;
            items[i].func_    = &barrier_work::execute;
            BOOST_TEST(!pool.post(&items[i]));

            // The first post starts the whole set. The barrier below
            // waits for every one of them, so a short start has to
            // fail the test rather than hang it.
            if (i == 0)
            {
                bool const full = pool.worker_count() == num_threads;
                BOOST_TEST(full);
                if (!full)
                {
                    give_up.store(true);
                    pool.shutdown();
                    return;
                }
            }
        }

        pool.shutdown();

        // All items completed — proves all 4 threads ran concurrently
        BOOST_TEST(done.load() == static_cast<int>(num_threads));
    }

    void testLazyWorkers()
    {
#if defined(__linux__)
        // A worker joined by an earlier suite can outlive its join in
        // /proc for a moment. A count that starts at zero says the
        // moment has passed, and from there only this context can add
        // to it.
        bool const countable = pool_thread_count() == 0;
#endif
        io_context ioc;
        auto* pool = ioc.find_service<detail::thread_pool>();

        // The context creates the service, not its workers.
        BOOST_TEST(pool != nullptr);
        BOOST_TEST(pool->worker_count() == 0);
#if defined(__linux__)
        // The counted skip keeps a dirty environment from passing as
        // a proof: what stands down is the check, visibly.
        BOOST_TEST(!countable || pool_thread_count() == 0);
#endif

        std::atomic<int> counter{0};
        test_work w;
        w.counter = &counter;
        w.func_   = &test_work::execute;
        BOOST_TEST(!pool->post(&w));
        BOOST_TEST(pool->worker_count() == 1);
        while (counter.load() == 0)
            std::this_thread::yield();

#if defined(__linux__)
        // The worker that ran the item is still parked on the queue.
        BOOST_TEST(!countable || pool_thread_count() == 1);
#endif
        pool->shutdown();
        BOOST_TEST(pool->worker_count() == 0);
    }

    void run()
    {
        testDrainOnShutdown();
        testShutdownWithNoWork();
        testPostAfterShutdown();
        testZeroThreads();
        testMultipleThreads();
        testLazyWorkers();
    }
};

TEST_SUITE(thread_pool_test, "boost.corosio.thread_pool");

} // namespace boost::corosio
