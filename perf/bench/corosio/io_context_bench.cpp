//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

capy::task<> increment_task( int64_t& counter )
{
    ++counter;
    co_return;
}

capy::task<> atomic_increment_task( std::atomic<int64_t>& counter )
{
    counter.fetch_add( 1, std::memory_order_relaxed );
    co_return;
}

bench::benchmark_result bench_single_threaded_post(
    perf::context_factory factory, double duration_s )
{
    perf::print_header( "Single-threaded Handler Post (Corosio)" );

    auto ioc = factory( 1 );
    auto ex = ioc->get_executor();
    int64_t counter = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration<double>( duration_s );

    while( std::chrono::steady_clock::now() < deadline )
    {
        for( int i = 0; i < batch_size; ++i )
            capy::run_async( ex )( increment_task( counter ) );

        ioc->poll();
        ioc->restart();
    }

    ioc->run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>( counter ) / elapsed;

    std::cout << "  Handlers:    " << counter << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << perf::format_rate( ops_per_sec ) << "\n";

    return bench::benchmark_result( "single_threaded_post" )
        .add( "handlers", static_cast<double>( counter ) )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

bench::benchmark_result bench_multithreaded_scaling(
    perf::context_factory factory, double duration_s, int max_threads )
{
    perf::print_header( "Multi-threaded Scaling (Corosio)" );

    bench::benchmark_result result( "multithreaded_scaling" );

    int constexpr batch_size = 100000;
    double baseline_ops = 0;

    for( int num_threads = 1; num_threads <= max_threads; num_threads *= 2 )
    {
        auto ioc = factory( max_threads );
        auto ex = ioc->get_executor();
        std::atomic<bool> running{ true };
        std::atomic<int64_t> counter{ 0 };

        // Seed initial batch
        for( int i = 0; i < batch_size; ++i )
            capy::run_async( ex )( atomic_increment_task( counter ) );

        perf::stopwatch sw;

        // Refill thread: keeps posting batches until duration expires
        std::thread feeder( [&]()
        {
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::duration<double>( duration_s );

            while( std::chrono::steady_clock::now() < deadline )
            {
                for( int i = 0; i < batch_size; ++i )
                    capy::run_async( ex )( atomic_increment_task( counter ) );
                std::this_thread::yield();
            }
            running.store( false, std::memory_order_relaxed );
        } );

        std::vector<std::thread> runners;
        for( int t = 0; t < num_threads; ++t )
            runners.emplace_back( [&ioc, &running]()
            {
                while( running.load( std::memory_order_relaxed ) )
                {
                    ioc->poll();
                    ioc->restart();
                }
                ioc->run();
            } );

        feeder.join();
        for( auto& t : runners )
            t.join();

        double elapsed = sw.elapsed_seconds();
        int64_t count = counter.load();
        double ops_per_sec = static_cast<double>( count ) / elapsed;

        std::cout << "  " << num_threads << " thread(s): "
                  << perf::format_rate( ops_per_sec );

        if( num_threads == 1 )
            baseline_ops = ops_per_sec;
        else if( baseline_ops > 0 )
            std::cout << " (speedup: " << std::fixed << std::setprecision( 2 )
                      << ( ops_per_sec / baseline_ops ) << "x)";
        std::cout << "\n";

        result.add( "threads_" + std::to_string( num_threads ) + "_ops_per_sec", ops_per_sec );
    }

    return result;
}

bench::benchmark_result bench_interleaved_post_run(
    perf::context_factory factory, double duration_s, int handlers_per_iteration )
{
    perf::print_header( "Interleaved Post/Run (Corosio)" );

    auto ioc = factory( 1 );
    auto ex = ioc->get_executor();
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration<double>( duration_s );

    while( std::chrono::steady_clock::now() < deadline )
    {
        for( int i = 0; i < handlers_per_iteration; ++i )
            capy::run_async( ex )( increment_task( counter ) );

        ioc->poll();
        ioc->restart();
    }

    ioc->run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>( counter ) / elapsed;

    std::cout << "  Handlers/iter:     " << handlers_per_iteration << "\n";
    std::cout << "  Total handlers:    " << counter << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << perf::format_rate( ops_per_sec ) << "\n";

    return bench::benchmark_result( "interleaved_post_run" )
        .add( "handlers_per_iteration", handlers_per_iteration )
        .add( "total_handlers", static_cast<double>( counter ) )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

bench::benchmark_result bench_concurrent_post_run(
    perf::context_factory factory, double duration_s, int num_threads )
{
    perf::print_header( "Concurrent Post and Run (Corosio)" );

    auto ioc = factory( num_threads );
    auto ex = ioc->get_executor();
    std::atomic<bool> running{ true };
    std::atomic<int64_t> counter{ 0 };

    int constexpr batch_size = 10000;

    perf::stopwatch sw;

    std::vector<std::thread> workers;
    for( int t = 0; t < num_threads; ++t )
    {
        workers.emplace_back( [&]()
        {
            while( running.load( std::memory_order_relaxed ) )
            {
                for( int i = 0; i < batch_size; ++i )
                    capy::run_async( ex )( atomic_increment_task( counter ) );
                ioc->poll();
                ioc->restart();
            }
            ioc->run();
        } );
    }

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    timer.join();
    for( auto& t : workers )
        t.join();

    double elapsed = sw.elapsed_seconds();
    int64_t count = counter.load();
    double ops_per_sec = static_cast<double>( count ) / elapsed;

    std::cout << "  Threads:           " << num_threads << "\n";
    std::cout << "  Total handlers:    " << count << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << perf::format_rate( ops_per_sec ) << "\n";

    return bench::benchmark_result( "concurrent_post_run" )
        .add( "threads", num_threads )
        .add( "total_handlers", static_cast<double>( count ) )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

} // anonymous namespace

void run_io_context_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        auto ioc = factory( 1 );
        auto ex = ioc->get_executor();
        int64_t counter = 0;
        for( int i = 0; i < 1000; ++i )
            capy::run_async( ex )( increment_task( counter ) );
        ioc->run();
    }

    if( run_all || std::strcmp( filter, "single_threaded" ) == 0 )
        collector.add( bench_single_threaded_post( factory, duration_s ) );

    if( run_all || std::strcmp( filter, "multithreaded" ) == 0 )
        collector.add( bench_multithreaded_scaling( factory, duration_s, 8 ) );

    if( run_all || std::strcmp( filter, "interleaved" ) == 0 )
        collector.add( bench_interleaved_post_run( factory, duration_s, 100 ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
        collector.add( bench_concurrent_post_run( factory, duration_s, 4 ) );
}

} // namespace corosio_bench
