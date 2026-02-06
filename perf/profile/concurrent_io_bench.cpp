//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Profiler workload: Concurrent I/O
//
// This program tests I/O completion handling under concurrent multi-threaded load.
// Run with a profiler (VTune, perf, VS Profiler) to identify hot spots in:
//   - IOCP completion distribution across threads
//   - ready_ flag CAS operations in overlapped_op
//   - Completion handler scheduling fairness
//   - Socket service contention
//
// Example command lines:
//   profile_concurrent_io --pairs 16 --threads 4   # Standard concurrent I/O
//   profile_concurrent_io --pairs 32 --threads 8   # High contention
//   profile_concurrent_io --pairs 4 --threads 1    # Baseline comparison

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
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

// Ping-pong coroutine: alternately write then read on a socket pair
// Passed by IILE parameters to avoid capture use-after-free
capy::task<> ping_pong(
    corosio::tcp_socket& sock_write,
    corosio::tcp_socket& sock_read,
    std::size_t buf_size,
    std::atomic<std::uint64_t>& ops,
    std::atomic<bool>& stop)
{
    std::vector<char> write_buf(buf_size, 'X');
    std::vector<char> read_buf(buf_size);

    while (!stop.load(std::memory_order_relaxed))
    {
        // Write
        auto [wec, wn] = co_await sock_write.write_some(
            capy::const_buffer(write_buf.data(), write_buf.size()));
        if (wec)
            co_return;

        // Read
        auto [rec, rn] = co_await sock_read.read_some(
            capy::mutable_buffer(read_buf.data(), read_buf.size()));
        if (rec)
            co_return;

        ops.fetch_add(2, std::memory_order_relaxed);
    }
}

//------------------------------------------------------------------------------

// Run the profiler workload for the specified duration
void run_workload(
    perf::context_factory factory,
    int duration_seconds,
    std::size_t buffer_size,
    int num_pairs,
    int num_threads)
{
    auto ioc = factory();
    std::atomic<std::uint64_t> ops{0};
    std::atomic<bool> stop{false};

    // Create socket pairs
    std::vector<std::pair<corosio::tcp_socket, corosio::tcp_socket>> pairs;
    pairs.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [a, b] = corosio::test::make_socket_pair(*ioc);
        a.set_no_delay(true);
        b.set_no_delay(true);
        pairs.emplace_back(std::move(a), std::move(b));
    }

    // Launch ping-pong on each pair
    for (auto& [a, b] : pairs)
    {
        capy::run_async(ioc->get_executor())(
            ping_pong(a, b, buffer_size, ops, stop));
    }

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_seconds);

    std::cout << "Running for " << duration_seconds << " seconds...\n";
    std::cout << "Pairs: " << num_pairs << ", Threads: " << num_threads
              << ", Buffer: " << buffer_size << " bytes\n\n";

    std::uint64_t last_count = 0;

    // Launch worker threads
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&]()
        {
            auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(2);

            while (std::chrono::steady_clock::now() < end_time)
            {
                ioc->run_for(std::chrono::milliseconds(100));

                // Only first thread reports progress
                auto now = std::chrono::steady_clock::now();
                if (now >= next_report)
                {
                    auto elapsed = std::chrono::duration<double>(now - start).count();
                    std::uint64_t current = ops.load(std::memory_order_relaxed);
                    double rate = static_cast<double>(current - last_count) / 2.0;

                    std::cout << "  [" << std::fixed << std::setprecision(0) << elapsed << "s] "
                              << perf::format_rate(rate) << " (" << current << " total)\n";

                    last_count = current;
                    next_report = now + std::chrono::seconds(2);
                }
            }
        });
    }

    // Wait for workers
    for (auto& w : workers)
        w.join();

    // Signal stop and cancel pending operations
    stop.store(true, std::memory_order_relaxed);
    for (auto& [a, b] : pairs)
    {
        a.cancel();
        b.cancel();
    }

    // Drain remaining work
    ioc->run();

    // Final stats
    auto total_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::uint64_t total = ops.load(std::memory_order_relaxed);
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
    std::size_t buffer_size,
    int num_pairs,
    int num_threads)
{
    std::cout << "Corosio Profiler Workload: Concurrent I/O\n";
    std::cout << "==========================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    std::cout << "Profile targets:\n";
    std::cout << "  - IOCP completion distribution across threads\n";
    std::cout << "  - ready_ flag CAS operations in overlapped_op\n";
    std::cout << "  - Completion handler scheduling fairness\n";
    std::cout << "  - Socket service contention\n\n";

    // Warmup
    std::cout << "Warming up (1 second)...\n";
    {
        auto ioc = factory();
        auto [a, b] = corosio::test::make_socket_pair(*ioc);
        a.set_no_delay(true);
        b.set_no_delay(true);

        std::atomic<std::uint64_t> warmup_ops{0};
        std::atomic<bool> warmup_stop{false};

        capy::run_async(ioc->get_executor())(
            ping_pong(a, b, 64, warmup_ops, warmup_stop));

        auto warmup_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < warmup_end)
            ioc->run_for(std::chrono::milliseconds(100));

        warmup_stop.store(true, std::memory_order_relaxed);
        a.cancel();
        b.cancel();
        ioc->run();
    }

    std::cout << "Warmup complete.\n\n";

    // Main workload
    run_workload(factory, duration, buffer_size, num_pairs, num_threads);

    std::cout << "\nWorkload complete.\n";
}

//------------------------------------------------------------------------------

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Profiler workload for concurrent I/O completion analysis.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>     Select I/O backend (default: platform default)\n";
    std::cout << "  --duration <secs>    Run duration in seconds (default: 10)\n";
    std::cout << "  --pairs <n>          Number of socket pairs (default: 16)\n";
    std::cout << "  --threads <n>        Runner threads (default: 4)\n";
    std::cout << "  --buffer <bytes>     Buffer size in bytes (default: 1024)\n";
    std::cout << "  --list               List available backends\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --pairs 16 --threads 4 --buffer 1024\n";
    std::cout << "\n";
    perf::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    int duration = 10;
    int num_pairs = 16;
    int num_threads = 4;
    std::size_t buffer_size = 1024;

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
        else if (std::strcmp(argv[i], "--pairs") == 0)
        {
            if (i + 1 < argc)
                num_pairs = std::atoi(argv[++i]);
            else
            {
                std::cerr << "Error: --pairs requires an argument\n";
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
        else if (std::strcmp(argv[i], "--buffer") == 0)
        {
            if (i + 1 < argc)
                buffer_size = static_cast<std::size_t>(std::atoi(argv[++i]));
            else
            {
                std::cerr << "Error: --buffer requires an argument\n";
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

    // Validate arguments
    if (num_pairs < 1)
    {
        std::cerr << "Error: --pairs must be >= 1\n";
        return 1;
    }
    if (num_threads < 1)
    {
        std::cerr << "Error: --threads must be >= 1\n";
        return 1;
    }
    if (buffer_size == 0)
    {
        std::cerr << "Error: --buffer must be > 0\n";
        return 1;
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = perf::default_backend_name();

    // Dispatch to the selected backend
    return perf::dispatch_backend(backend,
        [=](perf::context_factory factory, const char* name)
        {
            run_profiler_workload(factory, name, duration, buffer_size, num_pairs, num_threads);
        });
}
