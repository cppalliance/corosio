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
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Backend names for display
inline const char* default_backend_name()
{
#if defined(_WIN32)
    return "iocp";
#elif defined(__linux__)
    return "epoll";
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return "select";  // kqueue planned for future
#else
    return "select";
#endif
}

inline void print_available_backends()
{
    std::cout << "Available backends on this platform:\n";
#if defined(_WIN32)
    std::cout << "  iocp     - Windows I/O Completion Ports (default)\n";
#endif
#if defined(__linux__)
    std::cout << "  epoll    - Linux epoll (default)\n";
    std::cout << "  select   - POSIX select (portable)\n";
#elif !defined(_WIN32)
    std::cout << "  select   - POSIX select (default)\n";
#endif
    std::cout << "\nDefault backend: " << default_backend_name() << "\n";
}

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
template <typename Context>
void bench_single_threaded_post(int num_handlers)
{
    bench::print_header("Single-threaded Handler Post");

    Context ioc;
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
template <typename Context>
void bench_multithreaded_scaling(int num_handlers, int max_threads)
{
    bench::print_header("Multi-threaded Scaling");

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    for (int num_threads = 1; num_threads <= max_threads; num_threads *= 2)
    {
        Context ioc;
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
template <typename Context>
void bench_interleaved_post_run(int iterations, int handlers_per_iteration)
{
    bench::print_header("Interleaved Post/Run");

    Context ioc;
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
template <typename Context>
void bench_concurrent_post_run(int num_threads, int handlers_per_thread)
{
    bench::print_header("Concurrent Post and Run");

    Context ioc;
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

// Run all benchmarks for a specific context type
template <typename Context>
void run_all_benchmarks(const char* backend_name)
{
    std::cout << "Boost.Corosio io_context Benchmarks\n";
    std::cout << "====================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    // Warm up
    {
        Context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;
        for (int i = 0; i < 1000; ++i)
            ex.post(make_coro(counter));
        ioc.run();
    }

    // Run benchmarks
    bench_single_threaded_post<Context>(1000000);
    bench_multithreaded_scaling<Context>(1000000, 8);
    bench_interleaved_post_run<Context>(10000, 100);
    bench_concurrent_post_run<Context>(4, 250000);

    std::cout << "\nBenchmarks complete.\n";
}

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>   Select I/O backend (default: platform default)\n";
    std::cout << "  --list             List available backends\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0)
        {
            if (i + 1 < argc)
            {
                backend = argv[++i];
            }
            else
            {
                std::cerr << "Error: --backend requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            print_available_backends();
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

    // If no backend specified, use platform default
    if (!backend)
    {
        backend = default_backend_name();
    }

    // Run benchmarks for the selected backend
#if defined(__linux__)
    if (std::strcmp(backend, "epoll") == 0)
    {
        run_all_benchmarks<corosio::epoll_context>("epoll");
        return 0;
    }
#endif

#if !defined(_WIN32)
    if (std::strcmp(backend, "select") == 0)
    {
        run_all_benchmarks<corosio::select_context>("select");
        return 0;
    }
#endif

#if defined(_WIN32)
    if (std::strcmp(backend, "iocp") == 0)
    {
        run_all_benchmarks<corosio::iocp_context>("iocp");
        return 0;
    }
#endif

    // If we get here, the backend is not available
    std::cerr << "Error: Backend '" << backend << "' is not available on this platform.\n\n";
    print_available_backends();
    return 1;
}
