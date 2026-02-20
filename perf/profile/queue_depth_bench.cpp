//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Profiler workload: Queue Depth / Large Pending Queue
//
// This program tests dispatch efficiency with a large pending queue.
// Run with a profiler (VTune, perf, VS Profiler) to identify hot spots in:
//   - op_queue traversal cost
//   - completed_ops_ handling in do_one
//   - Memory access patterns (cache locality)
//   - Per-dispatch overhead at scale
//
// Example command lines:
//   profile_queue_depth --depth 100000           # Large queue, single thread
//   profile_queue_depth --depth 10000 --threads 4  # Moderate queue, multi-thread dispatch

#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>

#include "../common/backend_selection.hpp"
#include "../common/perf.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

// Empty coroutine - minimal work, maximizes framework overhead visibility
capy::task<>
empty_task(std::atomic<std::uint64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Run the profiler workload for the specified duration
void
run_workload(
    perf::context_factory factory,
    int duration_seconds,
    int queue_depth,
    int num_threads)
{
    auto ioc = factory();
    auto ex  = ioc->get_executor();
    std::atomic<std::uint64_t> counter{0};

    auto start       = std::chrono::steady_clock::now();
    auto end_time    = start + std::chrono::seconds(duration_seconds);
    auto next_report = start + std::chrono::seconds(2);

    std::cout << "Running for " << duration_seconds << " seconds...\n";
    std::cout << "Queue depth: " << queue_depth << ", Threads: " << num_threads
              << "\n\n";

    std::uint64_t last_count = 0;
    int iterations           = 0;

    while (std::chrono::steady_clock::now() < end_time)
    {
        // Fill the queue
        for (int i = 0; i < queue_depth; ++i)
            capy::run_async(ex)(empty_task(counter));

        // Dispatch with multiple threads if requested
        if (num_threads > 1)
        {
            std::vector<std::thread> workers;
            workers.reserve(num_threads);
            for (int t = 0; t < num_threads; ++t)
                workers.emplace_back([&]() { ioc->run(); });
            for (auto& w : workers)
                w.join();
        }
        else
        {
            ioc->run();
        }

        ioc->restart();
        ++iterations;

        // Progress report every 2 seconds
        auto now = std::chrono::steady_clock::now();
        if (now >= next_report)
        {
            auto elapsed = std::chrono::duration<double>(now - start).count();
            std::uint64_t current = counter.load(std::memory_order_relaxed);
            double rate = static_cast<double>(current - last_count) / 2.0;

            std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed
                      << "s] " << perf::format_rate(rate) << " (" << iterations
                      << " iterations)\n";

            last_count  = current;
            next_report = now + std::chrono::seconds(2);
        }
    }

    // Final stats
    auto total_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::uint64_t total = counter.load(std::memory_order_relaxed);
    double avg_rate     = static_cast<double>(total) / total_elapsed;

    std::cout << "\n=== Results ===\n";
    std::cout << "  Duration:   " << std::fixed << std::setprecision(2)
              << total_elapsed << " s\n";
    std::cout << "  Operations: " << total << "\n";
    std::cout << "  Iterations: " << iterations << "\n";
    std::cout << "  Avg rate:   " << perf::format_rate(avg_rate) << "\n";
}

void
run_profiler_workload(
    perf::context_factory factory,
    const char* backend_name,
    int duration,
    int queue_depth,
    int num_threads)
{
    std::cout << "Corosio Profiler Workload: Queue Depth\n";
    std::cout << "======================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    std::cout << "Profile targets:\n";
    std::cout << "  - op_queue traversal cost\n";
    std::cout << "  - completed_ops_ handling in do_one\n";
    std::cout << "  - Memory access patterns (cache locality)\n";
    std::cout << "  - Per-dispatch overhead at scale\n\n";

    // Warmup
    std::cout << "Warming up (1 second)...\n";
    {
        auto ioc = factory();
        auto ex  = ioc->get_executor();
        std::atomic<std::uint64_t> warmup_counter{0};

        auto warmup_end =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < warmup_end)
        {
            for (int i = 0; i < 1000; ++i)
                capy::run_async(ex)(empty_task(warmup_counter));
            ioc->poll();
            ioc->restart();
        }
    }

    std::cout << "Warmup complete.\n\n";

    // Main workload
    run_workload(factory, duration, queue_depth, num_threads);

    std::cout << "\nWorkload complete.\n";
}

void
print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout
        << "Profiler workload for large pending queue dispatch analysis.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>     Select I/O backend (default: platform "
                 "default)\n";
    std::cout
        << "  --duration <secs>    Run duration in seconds (default: 10)\n";
    std::cout << "  --depth <n>          Queue depth per iteration (default: "
                 "100000)\n";
    std::cout << "  --threads <n>        Dispatch threads (default: 1)\n";
    std::cout << "  --list               List available backends\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --depth 100000 --threads 1\n";
    std::cout << "\n";
    perf::print_available_backends();
}

int
main(int argc, char* argv[])
{
    const char* backend = nullptr;
    int duration        = 10;
    int queue_depth     = 100000;
    int num_threads     = 1;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0)
        {
            if (i + 1 < argc)
                backend = argv[++i];
            else
            {
                std::cerr << "Error: --backend requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--duration") == 0)
        {
            if (i + 1 < argc)
                duration = std::atoi(argv[++i]);
            else
            {
                std::cerr << "Error: --duration requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--depth") == 0)
        {
            if (i + 1 < argc)
                queue_depth = std::atoi(argv[++i]);
            else
            {
                std::cerr << "Error: --depth requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--threads") == 0)
        {
            if (i + 1 < argc)
                num_threads = std::atoi(argv[++i]);
            else
            {
                std::cerr << "Error: --threads requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            perf::print_available_backends();
            return 0;
        }
        else if (
            std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0)
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

    // Validate arguments
    if (queue_depth < 1)
    {
        std::cerr << "Error: --depth must be >= 1\n";
        return 1;
    }
    if (num_threads < 1)
    {
        std::cerr << "Error: --threads must be >= 1\n";
        return 1;
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = perf::default_backend_name();

    // Dispatch to the selected backend
    return perf::dispatch_backend(
        backend, [=](perf::context_factory factory, auto, const char* name) {
            run_profiler_workload(
                factory, name, duration, queue_depth, num_threads);
        });
}
