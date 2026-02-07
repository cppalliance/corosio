//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Profiler workload: Coroutine Post/Resume Path
//
// This program hammers the coroutine post/resume path for profiling.
// Run with a profiler (VTune, perf, VS Profiler) to identify hot spots in:
//   - run_async template instantiation
//   - post_handler allocation (new post_handler)
//   - PostQueuedCompletionStatus / IOCP posting
//   - GetQueuedCompletionStatus / dispatch loop
//   - coro.resume() cost

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>
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

//------------------------------------------------------------------------------

// Empty coroutine - minimal work, maximizes framework overhead visibility
capy::task<> empty_task(std::atomic<std::uint64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Coroutine with captured state - tests frame allocation scaling
template<std::size_t CaptureSize>
capy::task<> capture_task(std::atomic<std::uint64_t>& counter)
{
    // Force capture of N bytes
    [[maybe_unused]] char payload[CaptureSize];
    std::memset(payload, 0, CaptureSize);
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

//------------------------------------------------------------------------------

// Run the profiler workload for the specified duration
void run_workload(
    perf::context_factory factory,
    int duration_seconds,
    int batch_size,
    std::size_t capture_size)
{
    auto ioc = factory();
    auto ex = ioc->get_executor();
    std::atomic<std::uint64_t> counter{0};

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_seconds);
    auto next_report = start + std::chrono::seconds(2);

    std::cout << "Running for " << duration_seconds << " seconds...\n";
    std::cout << "Batch size: " << batch_size << ", Capture size: " << capture_size << " bytes\n\n";

    std::uint64_t last_count = 0;

    while (std::chrono::steady_clock::now() < end_time)
    {
        // Post a batch of coroutines
        for (int i = 0; i < batch_size; ++i)
        {
            switch (capture_size)
            {
            case 0:
                capy::run_async(ex)(empty_task(counter));
                break;
            case 64:
                capy::run_async(ex)(capture_task<64>(counter));
                break;
            case 256:
                capy::run_async(ex)(capture_task<256>(counter));
                break;
            case 1024:
                capy::run_async(ex)(capture_task<1024>(counter));
                break;
            default:
                capy::run_async(ex)(empty_task(counter));
                break;
            }
        }

        // Execute all pending work
        ioc->poll();
        ioc->restart();

        // Progress report every 2 seconds
        auto now = std::chrono::steady_clock::now();
        if (now >= next_report)
        {
            auto elapsed = std::chrono::duration<double>(now - start).count();
            std::uint64_t current = counter.load(std::memory_order_relaxed);
            double rate = static_cast<double>(current - last_count) / 2.0;

            std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed << "s] "
                      << perf::format_rate(rate) << " (" << current << " total)\n";

            last_count = current;
            next_report = now + std::chrono::seconds(2);
        }
    }

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

//------------------------------------------------------------------------------

void run_profiler_workload(
    perf::context_factory factory,
    const char* backend_name,
    int duration,
    int batch_size,
    std::size_t capture_size)
{
    std::cout << "Corosio Profiler Workload: Coroutine Post/Resume\n";
    std::cout << "================================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    std::cout << "Profile targets:\n";
    std::cout << "  - run_async / task machinery\n";
    std::cout << "  - post_handler allocation\n";
    std::cout << "  - IOCP posting (PostQueuedCompletionStatus)\n";
    std::cout << "  - Dispatch loop (GetQueuedCompletionStatus)\n";
    std::cout << "  - coro.resume()\n\n";

    // Warmup
    std::cout << "Warming up (1 second)...\n";
    {
        auto ioc = factory();
        auto ex = ioc->get_executor();
        std::atomic<std::uint64_t> warmup_counter{0};

        auto warmup_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
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
    run_workload(factory, duration, batch_size, capture_size);

    std::cout << "\nWorkload complete.\n";
}

//------------------------------------------------------------------------------

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Profiler workload for coroutine post/resume path analysis.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>     Select I/O backend (default: platform default)\n";
    std::cout << "  --duration <secs>    Run duration in seconds (default: 10)\n";
    std::cout << "  --batch <n>          Coroutines per poll cycle (default: 1000)\n";
    std::cout << "  --capture <bytes>    Captured state size: 0, 64, 256, 1024 (default: 0)\n";
    std::cout << "  --list               List available backends\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --duration 10 --batch 1000\n";
    std::cout << "\n";
    perf::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    int duration = 10;
    int batch_size = 1000;
    std::size_t capture_size = 0;

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
        else if (std::strcmp(argv[i], "--capture") == 0)
        {
            if (i + 1 < argc)
                capture_size = static_cast<std::size_t>(std::atoi(argv[++i]));
            else
            {
                std::cerr << "Error: --capture requires an argument\n";
                return 1;
            }
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

    // Validate capture size
    if (capture_size != 0 && capture_size != 64 && capture_size != 256 && capture_size != 1024)
    {
        std::cerr << "Error: --capture must be 0, 64, 256, or 1024\n";
        return 1;
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = perf::default_backend_name();

    // Dispatch to the selected backend
    return perf::dispatch_backend(backend,
        [=](perf::context_factory factory, const char* name)
        {
            run_profiler_workload(factory, name, duration, batch_size, capture_size);
        });
}
