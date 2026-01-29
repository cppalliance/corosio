//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/backend_selection.hpp"
#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Coroutine that increments a counter
capy::task<> increment_task(int& counter)
{
    ++counter;
    co_return;
}

// Coroutine that increments an atomic counter
capy::task<> atomic_increment_task(std::atomic<int>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Measures the raw throughput of posting and executing coroutines from a single
// thread. This establishes a baseline for the scheduler's best-case performance
// without any synchronization overhead. Useful for comparing coroutine dispatch
// efficiency against other async frameworks and identifying per-handler overhead.
template <typename Context>
bench::benchmark_result bench_single_threaded_post(int num_handlers)
{
    bench::print_header("Single-threaded Handler Post");

    Context ioc;
    auto ex = ioc.get_executor();
    int counter = 0;

    bench::stopwatch sw;

    for (int i = 0; i < num_handlers; ++i)
        capy::run_async(ex)(increment_task(counter));

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

// Measures how throughput scales when multiple threads call run() on the same
// io_context. Pre-posts all work, then times execution across 1, 2, 4, 8 threads.
// Reveals lock contention in the scheduler's work queue. Ideal scaling would show
// linear speedup; sub-linear or negative scaling indicates contention issues that
// may need strand-based partitioning in real applications.
template <typename Context>
bench::benchmark_result bench_multithreaded_scaling(int num_handlers, int max_threads)
{
    bench::print_header("Multi-threaded Scaling");

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    bench::benchmark_result result("multithreaded_scaling");
    result.add("handlers", num_handlers);

    double baseline_ops = 0;
    for (int num_threads = 1; num_threads <= max_threads; num_threads *= 2)
    {
        Context ioc;
        auto ex = ioc.get_executor();
        std::atomic<int> counter{0};

        // Post all handlers first
        for (int i = 0; i < num_handlers; ++i)
            capy::run_async(ex)(atomic_increment_task(counter));

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

        // Record per-thread results
        result.add("threads_" + std::to_string(num_threads) + "_ops_per_sec", ops_per_sec);

        if (counter.load() != num_handlers)
        {
            std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                      << ", got " << counter.load() << "\n";
        }
    }

    return result;
}

// Measures performance when posting and polling are interleaved, simulating a
// game loop or GUI event pump that processes available work each frame. Posts a
// batch of handlers, calls poll() to execute ready work, then repeats. Tests the
// efficiency of poll() with small work batches and frequent context restarts,
// which is common in latency-sensitive applications that can't block on run().
template <typename Context>
bench::benchmark_result bench_interleaved_post_run(int iterations, int handlers_per_iteration)
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
            capy::run_async(ex)(increment_task(counter));

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

// Measures performance under realistic concurrent load where multiple threads
// simultaneously post work AND execute it. This is the most stressful test for
// the scheduler's synchronization, as threads contend for both the submission
// and completion paths. Simulates server workloads where worker threads both
// generate new tasks and process existing ones, revealing producer-consumer
// bottlenecks.
template <typename Context>
bench::benchmark_result bench_concurrent_post_run(int num_threads, int handlers_per_thread)
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
                capy::run_async(ex)(atomic_increment_task(counter));
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

// Run benchmarks for a specific context type
template <typename Context>
void run_benchmarks(const char* backend_name, const char* output_file, const char* bench_filter)
{
    std::cout << "Boost.Corosio io_context Benchmarks\n";
    std::cout << "====================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    bench::result_collector collector(backend_name);

    bool run_all = !bench_filter || std::strcmp(bench_filter, "all") == 0;

    // Warm up
    {
        Context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;
        for (int i = 0; i < 1000; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.run();
    }

    // Run selected benchmarks
    if (run_all || std::strcmp(bench_filter, "single_threaded") == 0)
        collector.add(bench_single_threaded_post<Context>(1000000));

    if (run_all || std::strcmp(bench_filter, "multithreaded") == 0)
        collector.add(bench_multithreaded_scaling<Context>(1000000, 8));

    if (run_all || std::strcmp(bench_filter, "interleaved") == 0)
        collector.add(bench_interleaved_post_run<Context>(10000, 100));

    if (run_all || std::strcmp(bench_filter, "concurrent") == 0)
        collector.add(bench_concurrent_post_run<Context>(4, 250000));

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
    std::cout << "  --backend <name>   Select I/O backend (default: platform default)\n";
    std::cout << "  --bench <name>     Run only the specified benchmark\n";
    std::cout << "  --output <file>    Write JSON results to file\n";
    std::cout << "  --list             List available backends\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    std::cout << "Available benchmarks:\n";
    std::cout << "  single_threaded    Single-threaded handler post throughput\n";
    std::cout << "  multithreaded      Multi-threaded scaling test\n";
    std::cout << "  interleaved        Interleaved post/poll pattern\n";
    std::cout << "  concurrent         Concurrent post and run\n";
    std::cout << "  all                Run all benchmarks (default)\n";
    std::cout << "\n";
    bench::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    const char* output_file = nullptr;
    const char* bench_filter = nullptr;

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
        else if (std::strcmp(argv[i], "--bench") == 0)
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
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            bench::print_available_backends();
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
        backend = bench::default_backend_name();

    // Dispatch to the selected backend using a generic lambda
    return bench::dispatch_backend(backend,
        [=]<typename Context>(const char* name)
        {
            run_benchmarks<Context>(name, output_file, bench_filter);
        });
}
