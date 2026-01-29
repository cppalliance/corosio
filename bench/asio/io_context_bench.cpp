//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace asio = boost::asio;

// Measures the raw throughput of posting and executing handlers from a single
// thread. Establishes a baseline for Asio's scheduler performance without any
// synchronization overhead. Useful for comparing against Corosio's coroutine-based
// approach to understand the overhead difference between callbacks and coroutines.
bench::benchmark_result bench_single_threaded_post(int num_handlers)
{
    bench::print_header("Single-threaded Handler Post (Asio)");

    asio::io_context ioc;
    int counter = 0;

    bench::stopwatch sw;

    for (int i = 0; i < num_handlers; ++i)
    {
        asio::post(ioc, [&counter]() { ++counter; });
    }

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

    return bench::benchmark_result("single_threaded_post")
        .add("handlers", num_handlers)
        .add("elapsed_s", elapsed)
        .add("ops_per_sec", ops_per_sec);
}

// Measures how Asio's throughput scales when multiple threads call run() on the
// same io_context. Asio uses a mutex-protected queue, so this reveals contention
// characteristics. Compare against Corosio to evaluate different synchronization
// strategies. Sub-linear scaling is expected; the question is how gracefully
// performance degrades under thread pressure.
bench::benchmark_result bench_multithreaded_scaling(int num_handlers, int max_threads)
{
    bench::print_header("Multi-threaded Scaling (Asio)");

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    bench::benchmark_result result("multithreaded_scaling");
    result.add("handlers", num_handlers);

    double baseline_ops = 0;
    for (int num_threads = 1; num_threads <= max_threads; num_threads *= 2)
    {
        asio::io_context ioc;
        std::atomic<int> counter{0};

        // Post all handlers first
        for (int i = 0; i < num_handlers; ++i)
        {
            asio::post(ioc, [&counter]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }

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

        result.add("threads_" + std::to_string(num_threads) + "_ops_per_sec", ops_per_sec);

        if (counter.load() != num_handlers)
        {
            std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                      << ", got " << counter.load() << "\n";
        }
    }

    return result;
}

// Measures Asio performance when posting and polling are interleaved, simulating
// a game loop or GUI event pump. Tests poll() efficiency with small work batches
// and frequent restarts. Compare against Corosio to evaluate which framework
// handles this latency-sensitive pattern more efficiently.
bench::benchmark_result bench_interleaved_post_run(int iterations, int handlers_per_iteration)
{
    bench::print_header("Interleaved Post/Run (Asio)");

    asio::io_context ioc;
    int counter = 0;
    int total_handlers = iterations * handlers_per_iteration;

    bench::stopwatch sw;

    for (int iter = 0; iter < iterations; ++iter)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
        {
            asio::post(ioc, [&counter]() { ++counter; });
        }

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

    return bench::benchmark_result("interleaved_post_run")
        .add("iterations", iterations)
        .add("handlers_per_iteration", handlers_per_iteration)
        .add("total_handlers", total_handlers)
        .add("elapsed_s", elapsed)
        .add("ops_per_sec", ops_per_sec);
}

// Measures Asio performance under realistic concurrent load where multiple threads
// simultaneously post and execute work. This stresses Asio's synchronization
// primitives. Compare against Corosio to evaluate which framework handles
// producer-consumer workloads more efficiently.
bench::benchmark_result bench_concurrent_post_run(int num_threads, int handlers_per_thread)
{
    bench::print_header("Concurrent Post and Run (Asio)");

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
            {
                asio::post(ioc, [&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
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

    return bench::benchmark_result("concurrent_post_run")
        .add("threads", num_threads)
        .add("handlers_per_thread", handlers_per_thread)
        .add("total_handlers", total_handlers)
        .add("elapsed_s", elapsed)
        .add("ops_per_sec", ops_per_sec);
}

// Run benchmarks
void run_benchmarks(const char* output_file, const char* bench_filter)
{
    std::cout << "Boost.Asio io_context Benchmarks\n";
    std::cout << "=================================\n";

    bench::result_collector collector("asio");

    bool run_all = !bench_filter || std::strcmp(bench_filter, "all") == 0;

    // Warm up
    {
        asio::io_context ioc;
        int counter = 0;
        for (int i = 0; i < 1000; ++i)
            asio::post(ioc, [&counter]() { ++counter; });
        ioc.run();
    }

    // Run selected benchmarks
    if (run_all || std::strcmp(bench_filter, "single_threaded") == 0)
        collector.add(bench_single_threaded_post(1000000));

    if (run_all || std::strcmp(bench_filter, "multithreaded") == 0)
        collector.add(bench_multithreaded_scaling(1000000, 8));

    if (run_all || std::strcmp(bench_filter, "interleaved") == 0)
        collector.add(bench_interleaved_post_run(10000, 100));

    if (run_all || std::strcmp(bench_filter, "concurrent") == 0)
        collector.add(bench_concurrent_post_run(4, 250000));

    std::cout << "\nBenchmarks complete.\n";

    if (output_file)
    {
        if (collector.write_json(output_file))
            std::cout << "Results written to: " << output_file << "\n";
        else
            std::cerr << "Error: Failed to write results to: " << output_file << "\n";
    }
}

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --bench <name>     Run only the specified benchmark\n";
    std::cout << "  --output <file>    Write JSON results to file\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    std::cout << "Available benchmarks:\n";
    std::cout << "  single_threaded    Single-threaded handler post throughput\n";
    std::cout << "  multithreaded      Multi-threaded scaling test\n";
    std::cout << "  interleaved        Interleaved post/poll pattern\n";
    std::cout << "  concurrent         Concurrent post and run\n";
    std::cout << "  all                Run all benchmarks (default)\n";
}

int main(int argc, char* argv[])
{
    const char* output_file = nullptr;
    const char* bench_filter = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--bench") == 0)
        {
            if (i + 1 < argc)
            {
                bench_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --bench requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--output") == 0)
        {
            if (i + 1 < argc)
            {
                output_file = argv[++i];
            }
            else
            {
                std::cerr << "Error: --output requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    run_benchmarks(output_file, bench_filter);
    return 0;
}
