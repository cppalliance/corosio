//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Profiler workload: Multi-threaded Scheduler Contention
//
// This program hammers the scheduler with multiple threads posting and
// running coroutines concurrently. Run with a profiler to identify:
//   - dispatch_mutex_ lock contention
//   - InterlockedIncrement/Decrement on outstanding_work_
//   - Cache line bouncing between cores
//   - Unfair work distribution across threads
//
// Usage:
//
//   Balanced mode (default) - each thread posts and polls:
//     profile_scheduler_contention --threads 8 --batch 100
//
//   Post-only mode - profiles posting path (half threads post, half run):
//     profile_scheduler_contention --threads 8 --post-only
//
//   Run-only mode - isolates dispatch/completion path contention:
//     profile_scheduler_contention --threads 8 --run-only --batch 10000
//
// Options:
//   --threads N    Number of worker threads (default: 8)
//   --batch N      Coroutines per batch (default: 100)
//   --duration N   Run duration in seconds (default: 10)
//   --post-only    Half threads post (main included), half run
//   --run-only     Main posts continuously, all threads run

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
namespace capy = boost::capy;


enum class workload_mode
{
    balanced,   // Each thread posts and polls (default)
    post_only,  // All threads post, one thread runs
    run_only    // Pre-fill queue, all threads run
};

// Empty coroutine - minimal work, maximizes framework overhead visibility
capy::task<> empty_task(std::atomic<std::uint64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}


// Worker thread for balanced mode - posts and polls
void balanced_worker(
    corosio::basic_io_context& ioc,
    std::atomic<bool>& stop,
    std::atomic<std::uint64_t>& counter,
    int batch_size)
{
    auto ex = ioc.get_executor();
    while (!stop.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(empty_task(counter));
        ioc.poll();
    }
}

// Worker thread for post-only mode - only posts, never runs
void post_only_worker(
    corosio::basic_io_context& ioc,
    std::atomic<bool>& stop,
    std::atomic<std::uint64_t>& posted,
    int batch_size)
{
    auto ex = ioc.get_executor();
    while (!stop.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < batch_size; ++i)
        {
            capy::run_async(ex)(empty_task(posted));
        }
        // Yield to avoid spinning too hard
        std::this_thread::yield();
    }
}

// Runner thread for post-only mode - only runs, never posts
void post_only_runner(
    corosio::basic_io_context& ioc,
    std::atomic<bool>& stop)
{
    while (!stop.load(std::memory_order_relaxed))
    {
        auto n = ioc.poll();
        if (n == 0)
            std::this_thread::yield();
    }
    // Drain remaining work
    ioc.poll();
}

// Worker thread for run-only mode - only runs from pre-filled queue
void run_only_worker(
    corosio::basic_io_context& ioc,
    std::atomic<bool>& stop)
{
    while (!stop.load(std::memory_order_relaxed))
    {
        ioc.poll();
    }
}


void run_balanced_workload(
    perf::context_factory factory,
    int duration_seconds,
    int num_threads,
    int batch_size)
{
    auto ioc = factory();
    std::atomic<std::uint64_t> counter{0};
    std::atomic<bool> stop{false};

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_seconds);
    std::atomic<int> next_report_sec{2};

    std::cout << "Mode: balanced (each thread posts and polls)\n";
    std::cout << "Threads: " << num_threads << " (including main), Batch size: " << batch_size << "\n\n";

    std::atomic<std::uint64_t> last_count{0};

    // Launch N-1 worker threads (main thread will be the Nth worker)
    std::vector<std::thread> workers;
    workers.reserve(num_threads - 1);
    for (int t = 0; t < num_threads - 1; ++t)
    {
        workers.emplace_back([&]() {
            balanced_worker(*ioc, stop, counter, batch_size);
        });
    }

    // Main thread works too - no sleeping!
    auto ex = ioc->get_executor();
    std::uint64_t local_batches = 0;
    while (!stop.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(empty_task(counter));
        ioc->poll();
        ++local_batches;

        // Check time every 1000 batches to avoid syscall overhead
        if ((local_batches & 0x3FF) == 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time)
            {
                stop.store(true, std::memory_order_relaxed);
                break;
            }

            // Progress report (only main thread prints)
            auto elapsed = std::chrono::duration<double>(now - start).count();
            int elapsed_int = static_cast<int>(elapsed);
            int expected = next_report_sec.load(std::memory_order_relaxed);
            if (elapsed_int >= expected &&
                next_report_sec.compare_exchange_strong(expected, expected + 2))
            {
                std::uint64_t current = counter.load(std::memory_order_relaxed);
                std::uint64_t last = last_count.exchange(current, std::memory_order_relaxed);
                double rate = static_cast<double>(current - last) / 2.0;

                std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed << "s] "
                          << perf::format_rate(rate) << " (" << current << " total)\n";
            }
        }
    }

    // Stop workers
    for (auto& w : workers)
        w.join();

    // Final stats
    auto total_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::uint64_t total = counter.load(std::memory_order_relaxed);
    double avg_rate = static_cast<double>(total) / total_elapsed;

    std::cout << "\n=== Results ===\n";
    std::cout << "  Duration:   " << std::fixed << std::setprecision(2)
              << total_elapsed << " s\n";
    std::cout << "  Operations: " << total << "\n";
    std::cout << "  Avg rate:   " << perf::format_rate(avg_rate) << "\n";
}

void run_post_only_workload(
    perf::context_factory factory,
    int duration_seconds,
    int num_threads,
    int batch_size)
{
    auto ioc = factory();
    std::atomic<std::uint64_t> counter{0};
    std::atomic<bool> stop{false};

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_seconds);
    std::atomic<int> next_report_sec{2};

    // Split threads: main + half post, other half run
    int num_posters = (num_threads + 1) / 2;  // Round up, main is also a poster
    int num_runners = num_threads - num_posters;
    if (num_runners < 1) num_runners = 1;

    std::cout << "Mode: post-only (profile posting path contention)\n";
    std::cout << "Posters: " << num_posters << " (including main), Runners: " << num_runners
              << ", Batch size: " << batch_size << "\n" << std::endl;

    std::atomic<std::uint64_t> last_count{0};

    // Launch posting threads (main will be one more)
    std::vector<std::thread> posters;
    posters.reserve(num_posters - 1);
    for (int t = 0; t < num_posters - 1; ++t)
    {
        posters.emplace_back([&]() {
            post_only_worker(*ioc, stop, counter, batch_size);
        });
    }

    // Launch runner threads to consume work
    std::vector<std::thread> runners;
    runners.reserve(num_runners);
    for (int t = 0; t < num_runners; ++t)
    {
        runners.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed))
                ioc->poll();
            ioc->poll();  // Drain
        });
    }

    // Main thread posts - this is what we want to profile!
    auto ex = ioc->get_executor();
    std::uint64_t local_batches = 0;
    while (!stop.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(empty_task(counter));
        ++local_batches;

        // Check time every 256 batches
        if ((local_batches & 0xFF) == 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time)
            {
                stop.store(true, std::memory_order_relaxed);
                break;
            }

            // Progress report
            auto elapsed = std::chrono::duration<double>(now - start).count();
            int elapsed_int = static_cast<int>(elapsed);
            int expected = next_report_sec.load(std::memory_order_relaxed);
            if (elapsed_int >= expected &&
                next_report_sec.compare_exchange_strong(expected, expected + 2))
            {
                std::uint64_t current = counter.load(std::memory_order_relaxed);
                std::uint64_t last = last_count.exchange(current, std::memory_order_relaxed);
                double rate = static_cast<double>(current - last) / 2.0;

                std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed << "s] "
                          << perf::format_rate(rate) << " (" << current << " total)\n";
            }
        }
    }

    // Stop all threads
    for (auto& p : posters)
        p.join();
    for (auto& r : runners)
        r.join();

    // Final stats
    auto total_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::uint64_t total = counter.load(std::memory_order_relaxed);
    double avg_rate = static_cast<double>(total) / total_elapsed;

    std::cout << "\n=== Results ===\n";
    std::cout << "  Duration:   " << std::fixed << std::setprecision(2)
              << total_elapsed << " s\n";
    std::cout << "  Operations: " << total << "\n";
    std::cout << "  Avg rate:   " << perf::format_rate(avg_rate) << "\n";
}

void run_run_only_workload(
    perf::context_factory factory,
    int duration_seconds,
    int num_threads,
    int queue_depth)
{
    auto ioc = factory();
    std::atomic<std::uint64_t> counter{0};
    std::atomic<bool> stop{false};

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_seconds);
    std::atomic<int> next_report_sec{2};

    std::cout << "Mode: run-only (main posts, all threads dispatch)\n";
    std::cout << "Runner threads: " << num_threads << ", Queue depth: " << queue_depth << "\n\n";

    std::atomic<std::uint64_t> last_count{0};
    auto ex = ioc->get_executor();

    // Pre-fill the queue
    std::cout << "Pre-filling queue with " << queue_depth << " coroutines...\n";
    for (int i = 0; i < queue_depth; ++i)
        capy::run_async(ex)(empty_task(counter));

    // Launch runner threads
    std::vector<std::thread> runners;
    runners.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t)
    {
        runners.emplace_back([&]() {
            run_only_worker(*ioc, stop);
        });
    }

    // Main thread continuously refills - no sleeping!
    std::uint64_t local_refills = 0;
    while (!stop.load(std::memory_order_relaxed))
    {
        // Refill queue
        for (int i = 0; i < queue_depth; ++i)
            capy::run_async(ex)(empty_task(counter));
        ++local_refills;

        // Check time every 100 refills
        if ((local_refills & 0x3F) == 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= end_time)
            {
                stop.store(true, std::memory_order_relaxed);
                break;
            }

            // Progress report
            auto elapsed = std::chrono::duration<double>(now - start).count();
            int elapsed_int = static_cast<int>(elapsed);
            int expected = next_report_sec.load(std::memory_order_relaxed);
            if (elapsed_int >= expected &&
                next_report_sec.compare_exchange_strong(expected, expected + 2))
            {
                std::uint64_t current = counter.load(std::memory_order_relaxed);
                std::uint64_t last = last_count.exchange(current, std::memory_order_relaxed);
                double rate = static_cast<double>(current - last) / 2.0;

                std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed << "s] "
                          << perf::format_rate(rate) << " (" << current << " total)\n";
            }
        }
    }

    // Stop runners
    for (auto& r : runners)
        r.join();

    // Drain remaining
    ioc->poll();

    // Final stats
    auto total_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::uint64_t total = counter.load(std::memory_order_relaxed);
    double avg_rate = static_cast<double>(total) / total_elapsed;

    std::cout << "\n=== Results ===\n";
    std::cout << "  Duration:   " << std::fixed << std::setprecision(2)
              << total_elapsed << " s\n";
    std::cout << "  Operations: " << total << "\n";
    std::cout << "  Avg rate:   " << perf::format_rate(avg_rate) << "\n";
}


void run_profiler_workload(
    perf::context_factory factory,
    const char* backend_name,
    int duration,
    int num_threads,
    int batch_size,
    workload_mode mode)
{
    std::cout << "Corosio Profiler Workload: Scheduler Contention\n";
    std::cout << "================================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    std::cout << "Profile targets:\n";
    std::cout << "  - dispatch_mutex_ lock contention\n";
    std::cout << "  - outstanding_work_ atomic operations\n";
    std::cout << "  - Cache line bouncing between cores\n";
    std::cout << "  - Work distribution fairness\n" << std::endl;

    // Warmup - main thread participates, no sleeping
    std::cout << "Warming up (1 second)...\n";
    {
        auto ioc = factory();
        std::atomic<std::uint64_t> warmup_counter{0};
        std::atomic<bool> stop{false};

        auto warmup_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        std::vector<std::thread> warmup_threads;
        for (int t = 0; t < num_threads - 1; ++t)
        {
            warmup_threads.emplace_back([&]() {
                balanced_worker(*ioc, stop, warmup_counter, 100);
            });
        }

        // Main thread works during warmup too
        auto ex = ioc->get_executor();
        std::uint64_t local_batches = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            for (int i = 0; i < 100; ++i)
                capy::run_async(ex)(empty_task(warmup_counter));
            ioc->poll();
            ++local_batches;

            if ((local_batches & 0xFF) == 0)
            {
                if (std::chrono::steady_clock::now() >= warmup_end)
                {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        }

        for (auto& t : warmup_threads)
            t.join();
    }
    std::cout << "Warmup complete.\n" << std::endl;

    std::cout << "Running for " << duration << " seconds..." << std::endl;

    // Main workload
    switch (mode)
    {
    case workload_mode::balanced:
        run_balanced_workload(factory, duration, num_threads, batch_size);
        break;
    case workload_mode::post_only:
        run_post_only_workload(factory, duration, num_threads, batch_size);
        break;
    case workload_mode::run_only:
        run_run_only_workload(factory, duration, num_threads, batch_size);
        break;
    }

    std::cout << "\nWorkload complete.\n";
}


void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Profiler workload for multi-threaded scheduler contention analysis.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>     Select I/O backend (default: platform default)\n";
    std::cout << "  --duration <secs>    Run duration in seconds (default: 10)\n";
    std::cout << "  --threads <n>        Number of worker threads (default: 8)\n";
    std::cout << "  --batch <n>          Coroutines per thread per cycle (default: 100)\n";
    std::cout << "  --post-only          Profile posting path (half post, half run)\n";
    std::cout << "  --run-only           Profile dispatch path (main posts, all run)\n";
    std::cout << "  --list               List available backends\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\n";
    std::cout << "Modes:\n";
    std::cout << "  (default)   Each thread posts and polls - mixed contention\n";
    std::cout << "  --post-only Half threads post (including main), half run\n";
    std::cout << "  --run-only  Main posts, all threads run - dispatch contention\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --threads 8 --duration 10\n";
    std::cout << "  " << program_name << " --threads 16 --post-only\n";
    std::cout << "\n";
    perf::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    int duration = 10;
    int num_threads = 8;
    int batch_size = 100;
    workload_mode mode = workload_mode::balanced;

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
        else if (std::strcmp(argv[i], "--batch") == 0)
        {
            if (i + 1 < argc)
                batch_size = std::atoi(argv[++i]);
            else
            {
                std::cerr << "Error: --batch requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--post-only") == 0)
        {
            mode = workload_mode::post_only;
        }
        else if (std::strcmp(argv[i], "--run-only") == 0)
        {
            mode = workload_mode::run_only;
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            perf::print_available_backends();
            return 0;
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

    // Validate thread count
    if (num_threads < 1)
    {
        std::cerr << "Error: --threads must be at least 1\n";
        return 1;
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = perf::default_backend_name();

    // Dispatch to the selected backend
    return perf::dispatch_backend(backend,
        [=](perf::context_factory factory, const char* name)
        {
            run_profiler_workload(factory, name, duration, num_threads, batch_size, mode);
        });
}
