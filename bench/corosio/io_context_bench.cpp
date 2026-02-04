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
#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

capy::task<> increment_task( int& counter )
{
    ++counter;
    co_return;
}

capy::task<> atomic_increment_task( std::atomic<int>& counter )
{
    counter.fetch_add( 1, std::memory_order_relaxed );
    co_return;
}

template<typename Context>
bench::benchmark_result bench_single_threaded_post( int num_handlers )
{
    bench::print_header( "Single-threaded Handler Post" );

    Context ioc;
    auto ex = ioc.get_executor();
    int counter = 0;

    bench::stopwatch sw;

    for( int i = 0; i < num_handlers; ++i )
        capy::run_async( ex )( increment_task( counter ) );

    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>( num_handlers ) / elapsed;

    std::cout << "  Handlers:    " << num_handlers << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << bench::format_rate( ops_per_sec ) << "\n";

    if( counter != num_handlers )
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                  << ", got " << counter << "\n";
    }

    return bench::benchmark_result( "single_threaded_post" )
        .add( "handlers", num_handlers )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

template<typename Context>
bench::benchmark_result bench_multithreaded_scaling( int num_handlers, int max_threads )
{
    bench::print_header( "Multi-threaded Scaling" );

    std::cout << "  Handlers per test: " << num_handlers << "\n\n";

    bench::benchmark_result result( "multithreaded_scaling" );
    result.add( "handlers", num_handlers );

    double baseline_ops = 0;
    for( int num_threads = 1; num_threads <= max_threads; num_threads *= 2 )
    {
        Context ioc;
        auto ex = ioc.get_executor();
        std::atomic<int> counter{ 0 };

        for( int i = 0; i < num_handlers; ++i )
            capy::run_async( ex )( atomic_increment_task( counter ) );

        bench::stopwatch sw;

        std::vector<std::thread> runners;
        for( int t = 0; t < num_threads; ++t )
            runners.emplace_back( [&ioc]() { ioc.run(); } );

        for( auto& t : runners )
            t.join();

        double elapsed = sw.elapsed_seconds();
        double ops_per_sec = static_cast<double>( num_handlers ) / elapsed;

        std::cout << "  " << num_threads << " thread(s): "
                  << bench::format_rate( ops_per_sec );

        if( num_threads == 1 )
            baseline_ops = ops_per_sec;
        else if( baseline_ops > 0 )
            std::cout << " (speedup: " << std::fixed << std::setprecision( 2 )
                      << ( ops_per_sec / baseline_ops ) << "x)";
        std::cout << "\n";

        result.add( "threads_" + std::to_string( num_threads ) + "_ops_per_sec", ops_per_sec );

        if( counter.load() != num_handlers )
        {
            std::cerr << "  ERROR: counter mismatch! Expected " << num_handlers
                      << ", got " << counter.load() << "\n";
        }
    }

    return result;
}

template<typename Context>
bench::benchmark_result bench_interleaved_post_run( int iterations, int handlers_per_iteration )
{
    bench::print_header( "Interleaved Post/Run" );

    Context ioc;
    auto ex = ioc.get_executor();
    int counter = 0;
    int total_handlers = iterations * handlers_per_iteration;

    bench::stopwatch sw;

    for( int iter = 0; iter < iterations; ++iter )
    {
        for( int i = 0; i < handlers_per_iteration; ++i )
            capy::run_async( ex )( increment_task( counter ) );

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>( total_handlers ) / elapsed;

    std::cout << "  Iterations:        " << iterations << "\n";
    std::cout << "  Handlers/iter:     " << handlers_per_iteration << "\n";
    std::cout << "  Total handlers:    " << total_handlers << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << bench::format_rate( ops_per_sec ) << "\n";

    if( counter != total_handlers )
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << total_handlers
                  << ", got " << counter << "\n";
    }

    return bench::benchmark_result( "interleaved_post_run" )
        .add( "iterations", iterations )
        .add( "handlers_per_iteration", handlers_per_iteration )
        .add( "total_handlers", total_handlers )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

template<typename Context>
bench::benchmark_result bench_concurrent_post_run( int num_threads, int handlers_per_thread )
{
    bench::print_header( "Concurrent Post and Run" );

    Context ioc;
    auto ex = ioc.get_executor();
    std::atomic<int> counter{ 0 };
    int total_handlers = num_threads * handlers_per_thread;

    bench::stopwatch sw;

    std::vector<std::thread> workers;
    for( int t = 0; t < num_threads; ++t )
    {
        workers.emplace_back( [&ex, &ioc, &counter, handlers_per_thread]()
        {
            for( int i = 0; i < handlers_per_thread; ++i )
                capy::run_async( ex )( atomic_increment_task( counter ) );
            ioc.run();
        } );
    }

    for( auto& t : workers )
        t.join();

    double elapsed = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>( total_handlers ) / elapsed;

    std::cout << "  Threads:           " << num_threads << "\n";
    std::cout << "  Handlers/thread:   " << handlers_per_thread << "\n";
    std::cout << "  Total handlers:    " << total_handlers << "\n";
    std::cout << "  Elapsed:           " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:        " << bench::format_rate( ops_per_sec ) << "\n";

    if( counter.load() != total_handlers )
    {
        std::cerr << "  ERROR: counter mismatch! Expected " << total_handlers
                  << ", got " << counter.load() << "\n";
    }

    return bench::benchmark_result( "concurrent_post_run" )
        .add( "threads", num_threads )
        .add( "handlers_per_thread", handlers_per_thread )
        .add( "total_handlers", total_handlers )
        .add( "elapsed_s", elapsed )
        .add( "ops_per_sec", ops_per_sec );
}

} // anonymous namespace

template<typename Context>
void run_io_context_benchmarks(
    bench::result_collector& collector,
    char const* filter )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        Context ioc;
        auto ex = ioc.get_executor();
        int counter = 0;
        for( int i = 0; i < 1000; ++i )
            capy::run_async( ex )( increment_task( counter ) );
        ioc.run();
    }

    if( run_all || std::strcmp( filter, "single_threaded" ) == 0 )
        collector.add( bench_single_threaded_post<Context>( 5000000 ) );

    if( run_all || std::strcmp( filter, "multithreaded" ) == 0 )
        collector.add( bench_multithreaded_scaling<Context>( 5000000, 8 ) );

    if( run_all || std::strcmp( filter, "interleaved" ) == 0 )
        collector.add( bench_interleaved_post_run<Context>( 50000, 100 ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
        collector.add( bench_concurrent_post_run<Context>( 4, 1250000 ) );
}

// Explicit instantiations
#if BOOST_COROSIO_HAS_EPOLL
template void run_io_context_benchmarks<corosio::epoll_context>(
    bench::result_collector&, char const* );
#endif
#if BOOST_COROSIO_HAS_SELECT
template void run_io_context_benchmarks<corosio::select_context>(
    bench::result_collector&, char const* );
#endif
#if BOOST_COROSIO_HAS_IOCP
template void run_io_context_benchmarks<corosio::iocp_context>(
    bench::result_collector&, char const* );
#endif

} // namespace corosio_bench
