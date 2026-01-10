//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/platform_reactor.hpp>

#include <boost/capy/service_provider.hpp>

#include "test_suite.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace boost {
namespace corosio {

//------------------------------------------------
// Test work items

struct test_work : capy::executor_work
{
    std::atomic<int>* counter_;
    int value_;

    test_work(std::atomic<int>* counter, int value)
        : counter_(counter)
        , value_(value)
    {
    }

    void operator()() override
    {
        if (counter_)
            *counter_ += value_;
    }

    void destroy() override
    {
        delete this;
    }

    virtual ~test_work() = default;
};

struct counting_work : capy::executor_work
{
    std::atomic<int>* counter_;

    explicit counting_work(std::atomic<int>* counter)
        : counter_(counter)
    {
    }

    void operator()() override
    {
        if (counter_)
            (*counter_)++;
    }

    void destroy() override
    {
        delete this;
    }

    virtual ~counting_work() = default;
};

//------------------------------------------------
// Example io_context for testing

class test_io_context : public capy::service_provider
{
};

//------------------------------------------------

struct platform_reactor_test
{
    void
    testBasicSubmitProcess()
    {
        test_io_context ctx;
        auto& reactor = ctx.make_service<platform_reactor_single>();

        std::atomic<int> counter{0};

        // Submit some work
        reactor.submit(new test_work(&counter, 1));
        reactor.submit(new test_work(&counter, 2));
        reactor.submit(new test_work(&counter, 3));

        // Process the work
        reactor.process();

        BOOST_TEST_EQ(counter.load(), 6);
    }

    void
    testEmptyQueue()
    {
        test_io_context ctx;
        auto& reactor = ctx.make_service<platform_reactor_single>();

        // Process empty queue should be safe
        reactor.process();
        reactor.process();

        BOOST_TEST(true); // If we get here, it worked
    }

    void
    testShutdownCleansUp()
    {
        std::atomic<int> destroy_count{0};

        struct cleanup_work : capy::executor_work
        {
            std::atomic<int>* destroy_count_;

            explicit cleanup_work(std::atomic<int>* dc)
                : destroy_count_(dc)
            {
            }

            void operator()() override
            {
                // Should not be called during shutdown
                BOOST_TEST(false);
            }

            void destroy() override
            {
                if (destroy_count_)
                    (*destroy_count_)++;
                delete this;
            }

            virtual ~cleanup_work() = default;
        };

        {
            test_io_context ctx;
            auto& reactor = ctx.make_service<platform_reactor_single>();

            // Submit work that won't be processed
            reactor.submit(new cleanup_work(&destroy_count));
            reactor.submit(new cleanup_work(&destroy_count));
            reactor.submit(new cleanup_work(&destroy_count));

            // Let destructor call shutdown
        }

        BOOST_TEST_EQ(destroy_count.load(), 3);
    }

    void
    testProcessOrderFIFO()
    {
        test_io_context ctx;
        auto& reactor = ctx.make_service<platform_reactor_single>();

        std::vector<int> order;
        order.reserve(5);

        struct ordered_work : capy::executor_work
        {
            std::vector<int>* order_;
            int id_;

            ordered_work(std::vector<int>* order, int id)
                : order_(order)
                , id_(id)
            {
            }

            void operator()() override
            {
                if (order_)
                    order_->push_back(id_);
            }

            void destroy() override
            {
                delete this;
            }

            virtual ~ordered_work() = default;
        };

        // Submit in order
        for (int i = 1; i <= 5; ++i)
        {
            reactor.submit(new ordered_work(&order, i));
        }

        reactor.process();

        // Verify FIFO order
        BOOST_TEST_EQ(order.size(), 5u);
        for (int i = 0; i < 5; ++i)
        {
            BOOST_TEST_EQ(order[i], i + 1);
        }
    }

    void
    testMultipleProcessCalls()
    {
        test_io_context ctx;
        auto& reactor = ctx.make_service<platform_reactor_single>();

        std::atomic<int> counter{0};

        // Submit work in batches
        reactor.submit(new counting_work(&counter));
        reactor.submit(new counting_work(&counter));
        reactor.process();

        reactor.submit(new counting_work(&counter));
        reactor.process();

        reactor.submit(new counting_work(&counter));
        reactor.submit(new counting_work(&counter));
        reactor.submit(new counting_work(&counter));
        reactor.process();

        BOOST_TEST_EQ(counter.load(), 6);
    }

    void
    testThreadSafety()
    {
        test_io_context ctx;
        auto& reactor = ctx.make_service<platform_reactor_multi>();

        std::atomic<int> counter{0};
        const int num_threads = 4;
        const int work_per_thread = 100;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        // Launch threads that submit work
        for (int t = 0; t < num_threads; ++t)
        {
            threads.emplace_back([&reactor, &counter]() {
                for (int i = 0; i < work_per_thread; ++i)
                {
                    reactor.submit(new counting_work(&counter));
                }
            });
        }

        // Wait for all submissions
        for (auto& t : threads)
        {
            t.join();
        }

        // Process all submitted work
        const int total_work = num_threads * work_per_thread;
        for (int i = 0; i < total_work + 10; ++i)
        {
            reactor.process();
            if (counter == total_work)
                break;
        }

        BOOST_TEST_EQ(counter.load(), total_work);
    }

    void
    run()
    {
        testBasicSubmitProcess();
        testEmptyQueue();
        testShutdownCleansUp();
        testProcessOrderFIFO();
        testMultipleProcessCalls();
        testThreadSafety();
    }
};

TEST_SUITE(platform_reactor_test, "boost.corosio.platform_reactor");

} // namespace corosio
} // namespace boost
