//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Profiler workload: Small I/O Operations
//
// This program hammers the I/O completion path with small buffer operations
// for profiling. Run with a profiler (VTune, perf, VS Profiler) to identify
// hot spots in:
//   - overlapped_op allocation/completion
//   - IOCP completion handling
//   - Coroutine state machine transitions
//   - Per-operation framework overhead
//
// Example command lines:
//   profile_small_io --buffer 64 --pairs 1    # Single pair, tiny buffers (max overhead visibility)
//   profile_small_io --buffer 64 --pairs 8    # Multiple pairs, stress completion handling
//   profile_small_io --buffer 1024 --pairs 1  # Larger buffers, compare overhead ratio

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
    int num_pairs)
{
    auto ioc = factory( 1 );
    std::atomic<std::uint64_t> ops{0};
    std::atomic<bool> stop{false};

    // Create socket pairs and launch ping-pong coroutines
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
    auto next_report = start + std::chrono::seconds(2);

    std::cout << "Running for " << duration_seconds << " seconds...\n";
    std::cout << "Buffer size: " << buffer_size << " bytes, Pairs: " << num_pairs << "\n\n";

    std::uint64_t last_count = 0;

    // Run with periodic progress reports
    while (std::chrono::steady_clock::now() < end_time)
    {
        // Run for a short burst
        ioc->run_for(std::chrono::milliseconds(100));

        // Progress report every 2 seconds
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

    // Signal stop and let coroutines finish
    stop.store(true, std::memory_order_relaxed);

    // Cancel pending operations to unblock coroutines
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
    int num_pairs)
{
    std::cout << "Corosio Profiler Workload: Small I/O Operations\n";
    std::cout << "================================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    std::cout << "Profile targets:\n";
    std::cout << "  - overlapped_op allocation/completion\n";
    std::cout << "  - IOCP completion handling path\n";
    std::cout << "  - Coroutine state machine transitions\n";
    std::cout << "  - Per-operation framework overhead\n\n";

    // Warmup
    std::cout << "Warming up (1 second)...\n";
    {
        auto ioc = factory( 1 );
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
    run_workload(factory, duration, buffer_size, num_pairs);

    std::cout << "\nWorkload complete.\n";
}

//------------------------------------------------------------------------------

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Profiler workload for small I/O operation overhead analysis.\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>     Select I/O backend (default: platform default)\n";
    std::cout << "  --duration <secs>    Run duration in seconds (default: 10)\n";
    std::cout << "  --buffer <bytes>     Buffer size in bytes (default: 64)\n";
    std::cout << "  --pairs <n>          Number of socket pairs (default: 1)\n";
    std::cout << "  --list               List available backends\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  " << program_name << " --duration 10 --buffer 64 --pairs 4\n";
    std::cout << "\n";
    perf::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    int duration = 10;
    std::size_t buffer_size = 64;
    int num_pairs = 1;

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
    if (buffer_size == 0)
    {
        std::cerr << "Error: --buffer must be > 0\n";
        return 1;
    }
    if (num_pairs < 1)
    {
        std::cerr << "Error: --pairs must be >= 1\n";
        return 1;
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = perf::default_backend_name();

    // Dispatch to the selected backend
    return perf::dispatch_backend(backend,
        [=](perf::context_factory factory, const char* name)
        {
            run_profiler_workload(factory, name, duration, buffer_size, num_pairs);
        });
}
