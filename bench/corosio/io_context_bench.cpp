//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>

#include <atomic>
#include <coroutine>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

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

// Benchmark: Single-threaded handler posting rate
void bench_single_threaded_post(int num_handlers)
{
    bench::print_header("Single-threaded Handler Post");

    corosio::io_context ioc;
    auto ex = ioc.get_executor();
    int counter = 0;

    bench::stopwatch sw;

    for (int i = 0; i < num_handlers; ++i)
        ex.post(make_coro(counter));

    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>(num_handlers) / elapsed;

    std::cout << "  Handlers:    " << num_handlers << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << bench::format_rate(ops_per_sec) << "\n";

    if (counter != num_handlers)
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                  << ", got " << counter << "\n";
    }
}

// Benchmark: Multi-threaded scaling
void bench_multithreaded_scaling(int num_handlers, int max_threads)
{
    bench::print_header("Multi-threaded Scaling");

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    for (int num_threads = 1; num_threads <= max_threads; num_threads *= 2)
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};

        // Post all handlers first
        for (int i = 0; i < num_handlers; ++i)
            ex.post(make_atomic_coro(counter));

        bench::stopwatch sw;

        // Run with multiple threads
        std::vector<std::thread> runners;
        for (int t = 0; t < num_threads; ++t)
            runners.emplace_back([&ioc]() { ioc.run(); });

        for (auto& t : runners)
            t.join();

        double elapsed = sw.elapsed_seconds();
        double ops_per_sec = static_cast<double>(num_handlers) / elapsed;

        std::cout << "  " << num_threads << " thread(s): "
                  << bench::format_rate(ops_per_sec);

        if (num_threads > 1)
        {
            // Calculate speedup vs single-threaded baseline
            static double baseline_ops = 0;
            if (num_threads == 1)
                baseline_ops = ops_per_sec;
            else if (baseline_ops > 0)
                std::cout << " (speedup: " << std::fixed << std::setprecision(2)
                          << (ops_per_sec / baseline_ops) << "x)";
        }
        std::cout << "\n";

        if (counter.load() != num_handlers)
        {
            std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                      << ", got " << counter.load() << "\n";
        }
    }
}

// Benchmark: Post and run interleaved
void bench_interleaved_post_run(int iterations, int handlers_per_iteration)
{
    bench::print_header("Interleaved Post/Run");

    corosio::io_context ioc;
    auto ex = ioc.get_executor();
    int counter = 0;
    int total_handlers = iterations * handlers_per_iteration;

    bench::stopwatch sw;

    for (int iter = 0; iter < iterations; ++iter)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            ex.post(make_coro(counter));

        ioc.poll();
        ioc.restart();
    }

    // Run any remaining handlers
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>(total_handlers) / elapsed;

    std::cout << "  Iterations:        " << iterations << "\n";
    std::cout << "  Handlers/iter:     " << handlers_per_iteration << "\n";
    std::cout << "  Total handlers:    " << total_handlers << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << bench::format_rate(ops_per_sec) << "\n";

    if (counter != total_handlers)
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << total_handlers
                  << ", got " << counter << "\n";
    }
}

// Benchmark: Multi-threaded concurrent posting and running
void bench_concurrent_post_run(int num_threads, int handlers_per_thread)
{
    bench::print_header("Concurrent Post and Run");

    corosio::io_context ioc;
    auto ex = ioc.get_executor();
    std::atomic<int> counter{0};
    int total_handlers = num_threads * handlers_per_thread;

    bench::stopwatch sw;

    // Launch threads that both post and run
    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&ex, &ioc, &counter, handlers_per_thread]()
        {
            for (int i = 0; i < handlers_per_thread; ++i)
                ex.post(make_atomic_coro(counter));
            ioc.run();
        });
    }

    for (auto& t : workers)
        t.join();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>(total_handlers) / elapsed;

    std::cout << "  Threads:           " << num_threads << "\n";
    std::cout << "  Handlers/thread:   " << handlers_per_thread << "\n";
    std::cout << "  Total handlers:    " << total_handlers << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << bench::format_rate(ops_per_sec) << "\n";

    if (counter.load() != total_handlers)
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << total_handlers
                  << ", got " << counter.load() << "\n";
    }
}

int main()
{
    std::cout << "Boost.Corosio io_context Benchmarks\n";
    std::cout << "====================================\n";

    // Warm up
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;
        for (int i = 0; i < 1000; ++i)
            ex.post(make_coro(counter));
        ioc.run();
    }

    // Run benchmarks
    bench_single_threaded_post(1000000);
    bench_multithreaded_scaling(1000000, 8);
    bench_interleaved_post_run(10000, 100);
    bench_concurrent_post_run(4, 250000);

    std::cout << "\nBenchmarks complete.\n";
    return 0;
}
