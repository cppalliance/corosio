//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include "socket_utils.hpp"

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

#include "../common/benchmark.hpp"

namespace asio_bench {
namespace {

// Pattern C: coroutine loops check running flag
asio::awaitable<void> pingpong_client_task(
    tcp::socket& client,
    tcp::socket& server,
    std::size_t message_size,
    std::atomic<bool>& running,
    int64_t& iterations,
    perf::statistics& stats )
{
    std::vector<char> send_buf( message_size, 'P' );
    std::vector<char> recv_buf( message_size );

    try
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            co_await asio::async_write(
                client,
                asio::buffer( send_buf.data(), send_buf.size() ),
                asio::use_awaitable );

            co_await asio::async_read(
                server,
                asio::buffer( recv_buf.data(), recv_buf.size() ),
                asio::use_awaitable );

            co_await asio::async_write(
                server,
                asio::buffer( recv_buf.data(), recv_buf.size() ),
                asio::use_awaitable );

            co_await asio::async_read(
                client,
                asio::buffer( recv_buf.data(), recv_buf.size() ),
                asio::use_awaitable );

            double rtt_us = sw.elapsed_us();
            stats.add( rtt_us );
            ++iterations;
        }

        client.shutdown( tcp::socket::shutdown_send );
    }
    catch( std::exception const& ) {}
}

bench::benchmark_result bench_pingpong_latency( std::size_t message_size, double duration_s )
{
    std::cout << "  Message size: " << message_size << " bytes\n";

    asio::io_context ioc;
    auto [client, server] = make_socket_pair( ioc );

    std::atomic<bool> running{ true };
    int64_t iterations = 0;
    perf::statistics latency_stats;

    asio::co_spawn( ioc,
        pingpong_client_task(
            client, server, message_size, running, iterations, latency_stats ),
        asio::detached );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    timer.join();

    perf::print_latency_stats( latency_stats, "Round-trip latency" );
    std::cout << "  Iterations: " << iterations << "\n\n";

    client.close();
    server.close();

    return bench::benchmark_result( "pingpong_" + std::to_string( message_size ) )
        .add( "message_size", static_cast<double>( message_size ) )
        .add( "iterations", static_cast<double>( iterations ) )
        .add_latency_stats( "rtt", latency_stats );
}

bench::benchmark_result bench_concurrent_latency(
    int num_pairs, std::size_t message_size, double duration_s )
{
    std::cout << "  Concurrent pairs: " << num_pairs << ", ";
    std::cout << "Message size: " << message_size << " bytes\n";

    asio::io_context ioc;

    std::vector<tcp::socket> clients;
    std::vector<tcp::socket> servers;
    std::vector<perf::statistics> stats( num_pairs );
    std::vector<int64_t> iters( num_pairs, 0 );

    clients.reserve( num_pairs );
    servers.reserve( num_pairs );

    for( int i = 0; i < num_pairs; ++i )
    {
        auto [c, s] = make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    for( int p = 0; p < num_pairs; ++p )
    {
        asio::co_spawn( ioc,
            pingpong_client_task(
                clients[p], servers[p], message_size, running, iters[p], stats[p] ),
            asio::detached );
    }

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    timer.join();

    std::cout << "  Per-pair results:\n";
    for( int i = 0; i < num_pairs && i < 3; ++i )
    {
        std::cout << "    Pair " << i << ": mean="
                  << perf::format_latency( stats[i].mean() )
                  << ", p99=" << perf::format_latency( stats[i].p99() )
                  << ", iters=" << iters[i]
                  << "\n";
    }
    if( num_pairs > 3 )
        std::cout << "    ... (" << ( num_pairs - 3 ) << " more pairs)\n";

    double total_mean = 0;
    double total_p99 = 0;
    for( auto& s : stats )
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }
    std::cout << "  Average mean latency: "
              << perf::format_latency( total_mean / num_pairs ) << "\n";
    std::cout << "  Average p99 latency:  "
              << perf::format_latency( total_p99 / num_pairs ) << "\n\n";

    for( auto& c : clients )
        c.close();
    for( auto& s : servers )
        s.close();

    return bench::benchmark_result( "concurrent_" + std::to_string( num_pairs ) + "_pairs" )
        .add( "num_pairs", num_pairs )
        .add( "message_size", static_cast<double>( message_size ) )
        .add( "avg_mean_latency_us", total_mean / num_pairs )
        .add( "avg_p99_latency_us", total_p99 / num_pairs );
}

} // anonymous namespace

void run_socket_latency_benchmarks(
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        asio::io_context ioc;
        auto [c, s] = make_socket_pair( ioc );
        char buf[64] = {};
        for( int i = 0; i < 100; ++i )
        {
            asio::write( c, asio::buffer( buf ) );
            asio::read( s, asio::buffer( buf ) );
        }
        c.close();
        s.close();
    }

    std::vector<std::size_t> message_sizes = { 1, 64, 1024 };

    if( run_all || std::strcmp( filter, "pingpong" ) == 0 )
    {
        perf::print_header( "Ping-Pong Round-Trip Latency (Asio Coroutines)" );
        for( auto size : message_sizes )
            collector.add( bench_pingpong_latency( size, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Socket Pairs Latency (Asio Coroutines)" );
        collector.add( bench_concurrent_latency( 1, 64, duration_s ) );
        collector.add( bench_concurrent_latency( 4, 64, duration_s ) );
        collector.add( bench_concurrent_latency( 16, 64, duration_s ) );
    }
}

} // namespace asio_bench
