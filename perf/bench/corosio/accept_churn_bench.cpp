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
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

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

// Single connect/accept/1-byte-exchange/close loop. Measures the full
// per-connection lifecycle cost — fd allocation, TCP handshake, and teardown.
// Low throughput here indicates expensive socket setup or kernel overhead.
bench::benchmark_result bench_sequential_churn(
    perf::context_factory factory, double duration_s )
{
    perf::print_header( "Sequential Accept Churn (Corosio)" );

    auto ioc = factory();
    corosio::tcp_acceptor acc( *ioc );

    auto listen_ec = acc.listen( corosio::endpoint( corosio::ipv4_address::loopback(), 0 ) );
    if( listen_ec )
    {
        std::cerr << "  Listen failed: " << listen_ec.message() << "\n";
        return bench::benchmark_result( "sequential" )
            .add( "error", 1 );
    }

    auto ep = acc.local_endpoint();
    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    auto task = [&]() -> capy::task<>
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            corosio::tcp_socket client( *ioc );
            corosio::tcp_socket server( *ioc );
            client.open();

            // Spawn connect, await accept
            capy::run_async( ioc->get_executor() )(
                [&]() -> capy::task<>
                {
                    auto [ec] = co_await client.connect( ep );
                    (void)ec;
                }() );

            auto [aec] = co_await acc.accept( server );
            if( aec )
                co_return;

            // Exchange 1 byte
            char byte = 'X';
            auto [wec, wn] = co_await capy::write(
                client, capy::const_buffer( &byte, 1 ) );
            if( wec )
                co_return;

            char recv = 0;
            auto [rec, rn] = co_await capy::read(
                server, capy::mutable_buffer( &recv, 1 ) );
            if( rec )
                co_return;

            client.close();
            server.close();

            double latency_us = sw.elapsed_us();
            latency_stats.add( latency_us );
            ++cycles;
        }
    };

    perf::stopwatch total_sw;

    capy::run_async( ioc->get_executor() )( task() );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
    timer.join();

    double elapsed = total_sw.elapsed_seconds();
    double conns_per_sec = static_cast<double>( cycles ) / elapsed;

    std::cout << "  Cycles:      " << cycles << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << perf::format_rate( conns_per_sec ) << "\n";
    perf::print_latency_stats( latency_stats, "Cycle latency" );
    std::cout << "\n";

    acc.close();

    return bench::benchmark_result( "sequential" )
        .add( "cycles", static_cast<double>( cycles ) )
        .add( "elapsed_s", elapsed )
        .add( "conns_per_sec", conns_per_sec )
        .add_latency_stats( "cycle_latency", latency_stats );
}

// N independent accept loops on separate listeners. Reveals whether
// fd allocation or acceptor state scales linearly, and exposes any
// scheduler contention when multiple accept paths compete.
bench::benchmark_result bench_concurrent_churn(
    perf::context_factory factory, int num_loops, double duration_s )
{
    std::cout << "  Concurrent loops: " << num_loops << "\n";

    auto ioc = factory();
    std::atomic<bool> running{ true };
    std::vector<int64_t> cycle_counts( num_loops, 0 );
    std::vector<perf::statistics> stats( num_loops );

    // Each loop gets its own acceptor
    std::vector<corosio::tcp_acceptor> acceptors;
    acceptors.reserve( num_loops );
    for( int i = 0; i < num_loops; ++i )
    {
        acceptors.emplace_back( *ioc );
        auto ec = acceptors.back().listen(
            corosio::endpoint( corosio::ipv4_address::loopback(), 0 ) );
        if( ec )
        {
            std::cerr << "  Listen failed: " << ec.message() << "\n";
            return bench::benchmark_result( "concurrent_" + std::to_string( num_loops ) )
                .add( "error", 1 );
        }
    }

    auto loop_task = [&]( int idx ) -> capy::task<>
    {
        auto& acc = acceptors[idx];
        auto ep = acc.local_endpoint();

        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            corosio::tcp_socket client( *ioc );
            corosio::tcp_socket server( *ioc );
            client.open();

            capy::run_async( ioc->get_executor() )(
                [&]() -> capy::task<>
                {
                    auto [ec] = co_await client.connect( ep );
                    (void)ec;
                }() );

            auto [aec] = co_await acc.accept( server );
            if( aec )
                co_return;

            char byte = 'X';
            auto [wec, wn] = co_await capy::write(
                client, capy::const_buffer( &byte, 1 ) );
            if( wec )
                co_return;

            char recv = 0;
            auto [rec, rn] = co_await capy::read(
                server, capy::mutable_buffer( &recv, 1 ) );
            if( rec )
                co_return;

            client.close();
            server.close();

            stats[idx].add( sw.elapsed_us() );
            ++cycle_counts[idx];
        }
    };

    perf::stopwatch total_sw;

    for( int i = 0; i < num_loops; ++i )
        capy::run_async( ioc->get_executor() )( loop_task( i ) );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_cycles = 0;
    for( auto c : cycle_counts )
        total_cycles += c;

    double conns_per_sec = static_cast<double>( total_cycles ) / elapsed;

    double total_mean = 0;
    double total_p99 = 0;
    for( auto& s : stats )
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Total cycles: " << total_cycles << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( conns_per_sec ) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency( total_mean / num_loops ) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency( total_p99 / num_loops ) << "\n\n";

    for( auto& a : acceptors )
        a.close();

    return bench::benchmark_result( "concurrent_" + std::to_string( num_loops ) )
        .add( "num_loops", num_loops )
        .add( "total_cycles", static_cast<double>( total_cycles ) )
        .add( "conns_per_sec", conns_per_sec )
        .add( "avg_mean_latency_us", total_mean / num_loops )
        .add( "avg_p99_latency_us", total_p99 / num_loops );
}

// Burst N connects then accept all — stresses the listen backlog and
// batched fd creation. Reveals whether the acceptor handles connection
// storms gracefully or suffers from backlog overflow.
bench::benchmark_result bench_burst_churn(
    perf::context_factory factory, int burst_size, double duration_s )
{
    std::cout << "  Burst size: " << burst_size << "\n";

    auto ioc = factory();
    corosio::tcp_acceptor acc( *ioc );

    auto listen_ec = acc.listen( corosio::endpoint( corosio::ipv4_address::loopback(), 0 ) );
    if( listen_ec )
    {
        std::cerr << "  Listen failed: " << listen_ec.message() << "\n";
        return bench::benchmark_result( "burst_" + std::to_string( burst_size ) )
            .add( "error", 1 );
    }

    auto ep = acc.local_endpoint();
    std::atomic<bool> running{ true };
    int64_t total_accepted = 0;
    perf::statistics burst_stats;

    auto task = [&]() -> capy::task<>
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            std::vector<corosio::tcp_socket> clients;
            std::vector<corosio::tcp_socket> servers;
            clients.reserve( burst_size );
            servers.reserve( burst_size );

            // Spawn all connects
            for( int i = 0; i < burst_size; ++i )
            {
                clients.emplace_back( *ioc );
                clients.back().open();
                capy::run_async( ioc->get_executor() )(
                    [&c = clients.back(), ep]() -> capy::task<>
                    {
                        auto [ec] = co_await c.connect( ep );
                        (void)ec;
                    }() );
            }

            // Accept all
            for( int i = 0; i < burst_size; ++i )
            {
                servers.emplace_back( *ioc );
                auto [aec] = co_await acc.accept( servers.back() );
                if( aec )
                    co_return;
                ++total_accepted;
            }

            // Close all
            for( auto& c : clients )
                c.close();
            for( auto& s : servers )
                s.close();

            burst_stats.add( sw.elapsed_us() );
        }
    };

    perf::stopwatch total_sw;

    capy::run_async( ioc->get_executor() )( task() );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();
    double accepts_per_sec = static_cast<double>( total_accepted ) / elapsed;

    std::cout << "    Total accepted: " << total_accepted << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Accept rate: " << perf::format_rate( accepts_per_sec ) << "\n";
    perf::print_latency_stats( burst_stats, "Burst latency" );
    std::cout << "\n";

    acc.close();

    return bench::benchmark_result( "burst_" + std::to_string( burst_size ) )
        .add( "burst_size", burst_size )
        .add( "total_accepted", static_cast<double>( total_accepted ) )
        .add( "accepts_per_sec", accepts_per_sec )
        .add_latency_stats( "burst_latency", burst_stats );
}

} // anonymous namespace

void run_accept_churn_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    if( run_all || std::strcmp( filter, "sequential" ) == 0 )
        collector.add( bench_sequential_churn( factory, duration_s ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Accept Churn (Corosio)" );
        collector.add( bench_concurrent_churn( factory, 1, duration_s ) );
        collector.add( bench_concurrent_churn( factory, 4, duration_s ) );
        collector.add( bench_concurrent_churn( factory, 16, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "burst" ) == 0 )
    {
        perf::print_header( "Burst Accept Churn (Corosio)" );
        collector.add( bench_burst_churn( factory, 10, duration_s ) );
        collector.add( bench_burst_churn( factory, 100, duration_s ) );
    }
}

} // namespace corosio_bench
