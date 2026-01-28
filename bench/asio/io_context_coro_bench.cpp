//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// This benchmark uses coroutines (like Corosio) for a fair comparison,
// rather than plain callbacks.

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace asio = boost::asio;

// Coroutine that increments a counter
asio::awaitable<void> increment_task(int& counter)
{
    ++counter;
    co_return;
}

// Coroutine that increments an atomic counter
asio::awaitable<void> atomic_increment_task(std::atomic<int>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Benchmark: Single-threaded coroutine posting rate
void bench_single_threaded_post(int num_handlers)
{
    bench::print_header("Single-threaded Coroutine Post (Asio)");

    asio::io_context ioc;
    int counter = 0;

    bench::stopwatch sw;

    for (int i = 0; i < num_handlers; ++i)
        asio::co_spawn(ioc, increment_task(counter), asio::detached);

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

// Benchmark: Multi-threaded scaling with coroutines
void bench_multithreaded_scaling(int num_handlers, int max_threads)
{
    bench::print_header("Multi-threaded Scaling (Asio Coroutines)");

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    double baseline_ops = 0;

    for (int num_threads = 1; num_threads <= max_threads; num_threads *= 2)
    {
        asio::io_context ioc;
        std::atomic<int> counter{0};

        // Post all coroutines first
        for (int i = 0; i < num_handlers; ++i)
            asio::co_spawn(ioc, atomic_increment_task(counter), asio::detached);

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

        if (num_threads == 1)
            baseline_ops = ops_per_sec;
        else if (baseline_ops > 0)
            std::cout << " (speedup: " << std::fixed << std::setprecision(2)
                      << (ops_per_sec / baseline_ops) << "x)";

        std::cout << "\n";

        if (counter.load() != num_handlers)
        {
            std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                      << ", got " << counter.load() << "\n";
        }
    }
}

// Benchmark: Interleaved post and run with coroutines
void bench_interleaved_post_run(int iterations, int handlers_per_iteration)
{
    bench::print_header("Interleaved Post/Run (Asio Coroutines)");

    asio::io_context ioc;
    int counter = 0;
    int total_handlers = iterations * handlers_per_iteration;

    bench::stopwatch sw;

    for (int iter = 0; iter < iterations; ++iter)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            asio::co_spawn(ioc, increment_task(counter), asio::detached);

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

// Benchmark: Concurrent posting and running with coroutines
void bench_concurrent_post_run(int num_threads, int handlers_per_thread)
{
    bench::print_header("Concurrent Post and Run (Asio Coroutines)");

    asio::io_context ioc;
    std::atomic<int> counter{0};
    int total_handlers = num_threads * handlers_per_thread;

    bench::stopwatch sw;

    // Launch threads that both post and run
    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&ioc, &counter, handlers_per_thread]()
        {
            for (int i = 0; i < handlers_per_thread; ++i)
                asio::co_spawn(ioc, atomic_increment_task(counter), asio::detached);
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
    std::cout << "Boost.Asio io_context Benchmarks (Coroutine Version)\n";
    std::cout << "====================================================\n";
    std::cout << "Using coroutines for fair comparison with Corosio\n";

    // Warm up
    {
        asio::io_context ioc;
        int counter = 0;
        for (int i = 0; i < 1000; ++i)
            asio::co_spawn(ioc, increment_task(counter), asio::detached);
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
