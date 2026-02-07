//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include "../socket_utils.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace asio_bench {
namespace {

// Single connect/accept/1-byte-exchange/close loop. Measures the full
// per-connection lifecycle cost — fd allocation, TCP handshake, and teardown.
bench::benchmark_result bench_sequential_churn( double duration_s )
{
    perf::print_header( "Sequential Accept Churn (Asio Coroutines)" );

    asio::io_context ioc;
    tcp::acceptor acc( ioc, tcp::endpoint( tcp::v4(), 0 ) );
    acc.set_option( tcp::acceptor::reuse_address( true ) );
    auto ep = tcp::endpoint( asio::ip::address_v4::loopback(), acc.local_endpoint().port() );

    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    auto task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( running.load( std::memory_order_relaxed ) )
            {
                perf::stopwatch sw;

                auto client = std::make_unique<tcp::socket>( ioc );
                auto server = std::make_unique<tcp::socket>( ioc );

                // Spawn connect, await accept
                asio::co_spawn( ioc,
                    [&client, ep]() -> asio::awaitable<void>
                    {
                        co_await client->async_connect( ep, asio::use_awaitable );
                    }(),
                    asio::detached );

                *server = co_await acc.async_accept( asio::use_awaitable );

                // Exchange 1 byte
                char byte = 'X';
                co_await asio::async_write(
                    *client, asio::buffer( &byte, 1 ), asio::use_awaitable );

                char recv = 0;
                co_await asio::async_read(
                    *server, asio::buffer( &recv, 1 ), asio::use_awaitable );

                client->close();
                server->close();

                double latency_us = sw.elapsed_us();
                latency_stats.add( latency_us );
                ++cycles;
            }
        }
        catch( std::exception const& ) {}
    };

    perf::stopwatch total_sw;

    asio::co_spawn( ioc, task(), asio::detached );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
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
bench::benchmark_result bench_concurrent_churn( int num_loops, double duration_s )
{
    std::cout << "  Concurrent loops: " << num_loops << "\n";

    asio::io_context ioc;
    std::atomic<bool> running{ true };
    std::vector<int64_t> cycle_counts( num_loops, 0 );
    std::vector<perf::statistics> stats( num_loops );

    // Each loop gets its own acceptor
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    acceptors.reserve( num_loops );
    for( int i = 0; i < num_loops; ++i )
    {
        acceptors.push_back( std::make_unique<tcp::acceptor>(
            ioc, tcp::endpoint( tcp::v4(), 0 ) ) );
        acceptors.back()->set_option( tcp::acceptor::reuse_address( true ) );
    }

    auto loop_task = [&]( int idx ) -> asio::awaitable<void>
    {
        auto& acc = *acceptors[idx];
        auto ep = tcp::endpoint( asio::ip::address_v4::loopback(), acc.local_endpoint().port() );

        try
        {
            while( running.load( std::memory_order_relaxed ) )
            {
                perf::stopwatch sw;

                auto client = std::make_unique<tcp::socket>( ioc );
                auto server = std::make_unique<tcp::socket>( ioc );

                asio::co_spawn( ioc,
                    [&client, ep]() -> asio::awaitable<void>
                    {
                        co_await client->async_connect( ep, asio::use_awaitable );
                    }(),
                    asio::detached );

                *server = co_await acc.async_accept( asio::use_awaitable );

                char byte = 'X';
                co_await asio::async_write(
                    *client, asio::buffer( &byte, 1 ), asio::use_awaitable );

                char recv = 0;
                co_await asio::async_read(
                    *server, asio::buffer( &recv, 1 ), asio::use_awaitable );

                client->close();
                server->close();

                stats[idx].add( sw.elapsed_us() );
                ++cycle_counts[idx];
            }
        }
        catch( std::exception const& ) {}
    };

    perf::stopwatch total_sw;

    for( int i = 0; i < num_loops; ++i )
        asio::co_spawn( ioc, loop_task( i ), asio::detached );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
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
        a->close();

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
bench::benchmark_result bench_burst_churn( int burst_size, double duration_s )
{
    std::cout << "  Burst size: " << burst_size << "\n";

    asio::io_context ioc;
    tcp::acceptor acc( ioc, tcp::endpoint( tcp::v4(), 0 ) );
    acc.set_option( tcp::acceptor::reuse_address( true ) );
    auto ep = tcp::endpoint( asio::ip::address_v4::loopback(), acc.local_endpoint().port() );

    std::atomic<bool> running{ true };
    int64_t total_accepted = 0;
    perf::statistics burst_stats;

    auto task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( running.load( std::memory_order_relaxed ) )
            {
                perf::stopwatch sw;

                std::vector<std::unique_ptr<tcp::socket>> clients;
                std::vector<tcp::socket> servers;
                clients.reserve( burst_size );
                servers.reserve( burst_size );

                // Spawn all connects
                for( int i = 0; i < burst_size; ++i )
                {
                    clients.push_back( std::make_unique<tcp::socket>( ioc ) );
                    asio::co_spawn( ioc,
                        [&c = *clients.back(), ep]() -> asio::awaitable<void>
                        {
                            co_await c.async_connect( ep, asio::use_awaitable );
                        }(),
                        asio::detached );
                }

                // Accept all
                for( int i = 0; i < burst_size; ++i )
                {
                    servers.push_back( co_await acc.async_accept( asio::use_awaitable ) );
                    ++total_accepted;
                }

                // Close all
                for( auto& c : clients )
                    c->close();
                for( auto& s : servers )
                    s.close();

                burst_stats.add( sw.elapsed_us() );
            }
        }
        catch( std::exception const& ) {}
    };

    perf::stopwatch total_sw;

    asio::co_spawn( ioc, task(), asio::detached );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
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
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    if( run_all || std::strcmp( filter, "sequential" ) == 0 )
        collector.add( bench_sequential_churn( duration_s ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Accept Churn (Asio Coroutines)" );
        collector.add( bench_concurrent_churn( 1, duration_s ) );
        collector.add( bench_concurrent_churn( 4, duration_s ) );
        collector.add( bench_concurrent_churn( 16, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "burst" ) == 0 )
    {
        perf::print_header( "Burst Accept Churn (Asio Coroutines)" );
        collector.add( bench_burst_churn( 10, duration_s ) );
        collector.add( bench_burst_churn( 100, duration_s ) );
    }
}

} // namespace asio_bench
