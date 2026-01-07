//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <corosio/platform_reactor.hpp>
#include <capy/service_provider.hpp>

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

//------------------------------------------------
// Test work items

struct test_work : capy::executor_work
{
    std::atomic<int>* counter_;
    int value_;

    test_work(std::atomic<int>* counter, int value) : counter_(counter), value_(value) {}

    void operator()() override
    {
        if(counter_)
            *counter_ += value_;
    }

    void destroy() override { delete this; }
    virtual ~test_work() = default;
};

struct counting_work : capy::executor_work
{
    std::atomic<int>* counter_;

    explicit counting_work(std::atomic<int>* counter) : counter_(counter) {}

    void operator()() override
    {
        if(counter_)
            (*counter_)++;
    }

    void destroy() override { delete this; }
    virtual ~counting_work() = default;
};

//------------------------------------------------
// Example io_context

class io_context : public capy::service_provider
{
public:
};

//------------------------------------------------

int main()
{
    std::cout << "=== Test 1: Basic submit and process ===\n";
    {
        io_context ctx;
        auto& reactor = ctx.make_service<corosio::platform_reactor_single>();

        std::atomic<int> counter{0};

        // Submit some work
        reactor.submit(new test_work(&counter, 1));
        reactor.submit(new test_work(&counter, 2));
        reactor.submit(new test_work(&counter, 3));

        // Process the work
        reactor.process();

        assert(counter == 6);
        std::cout << "Counter: " << counter << " (expected 6)\n";
    }

    std::cout << "\n=== Test 2: Empty queue ===\n";
    {
        io_context ctx;
        auto& reactor = ctx.make_service<corosio::platform_reactor_single>();

        // Process empty queue should be safe
        reactor.process();
        reactor.process();

        std::cout << "Empty queue processed successfully\n";
    }

    std::cout << "\n=== Test 3: Shutdown cleans up work ===\n";
    {
        std::atomic<int> destroy_count{0};

        struct cleanup_work : capy::executor_work
        {
            std::atomic<int>* destroy_count_;

            explicit cleanup_work(std::atomic<int>* dc) : destroy_count_(dc) {}

            void operator()() override
            {
                // Should not be called during shutdown
                assert(false);
            }

            void destroy() override
            {
                if(destroy_count_)
                    (*destroy_count_)++;
                delete this;
            }
            virtual ~cleanup_work() = default;
        };

        {
            io_context ctx;
            auto& reactor = ctx.make_service<corosio::platform_reactor_single>();

            // Submit work that won't be processed
            reactor.submit(new cleanup_work(&destroy_count));
            reactor.submit(new cleanup_work(&destroy_count));
            reactor.submit(new cleanup_work(&destroy_count));

            // Let destructor call shutdown
        }

        assert(destroy_count == 3);
        std::cout << "Destroy count: " << destroy_count << " (expected 3)\n";
    }
    std::cout << "Shutdown completed\n";

    std::cout << "\n=== Test 4: Process order (FIFO) ===\n";
    {
        io_context ctx;
        auto& reactor = ctx.make_service<corosio::platform_reactor_single>();

        std::vector<int> order;
        order.reserve(5);

        struct ordered_work : capy::executor_work
        {
            std::vector<int>* order_;
            int id_;

            ordered_work(std::vector<int>* order, int id) : order_(order), id_(id) {}

            void operator()() override
            {
                if(order_)
                    order_->push_back(id_);
            }

            void destroy() override { delete this; }
            virtual ~ordered_work() = default;
        };

        // Submit in order
        for(int i = 1; i <= 5; ++i)
        {
            reactor.submit(new ordered_work(&order, i));
        }

        reactor.process();

        // Verify FIFO order
        assert(order.size() == 5);
        for(int i = 0; i < 5; ++i)
        {
            assert(order[i] == i + 1);
        }
        std::cout << "Order: ";
        for(int id : order)
            std::cout << id << " ";
        std::cout << "(expected 1 2 3 4 5)\n";
    }

    std::cout << "\n=== Test 5: Multiple process calls ===\n";
    {
        io_context ctx;
        auto& reactor = ctx.make_service<corosio::platform_reactor_single>();

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

        assert(counter == 6);
        std::cout << "Counter: " << counter << " (expected 6)\n";
    }

    std::cout << "\n=== Test 6: Thread safety (submit from multiple threads) ===\n";
    {
        io_context ctx;
        auto& reactor = ctx.make_service<corosio::platform_reactor_multi>();

        std::atomic<int> counter{0};
        const int num_threads = 4;
        const int work_per_thread = 100;

        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        // Launch threads that submit work
        for(int t = 0; t < num_threads; ++t)
        {
            threads.emplace_back([&reactor, &counter]() {
                for(int i = 0; i < work_per_thread; ++i)
                {
                    reactor.submit(new counting_work(&counter));
                }
            });
        }

        // Wait for all submissions
        for(auto& t : threads)
        {
            t.join();
        }

        // Process all submitted work
        // We know exactly how much work was submitted, so process enough times
        const int total_work = num_threads * work_per_thread;
        for(int i = 0; i < total_work + 10; ++i) // +10 for safety margin
        {
            reactor.process();
            if(counter == total_work)
                break;
        }

        assert(counter == total_work);
        std::cout << "Counter: " << counter << " (expected " << total_work << ")\n";
    }

    std::cout << "\n=== All tests passed! ===\n";
    return 0;
}
