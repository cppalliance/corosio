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

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

struct pingpong_op
{
    enum phase { write_client, read_server, write_server, read_client };

    tcp_socket& client;
    tcp_socket& server;
    std::vector<char> send_buf;
    std::vector<char> recv_buf;
    std::atomic<bool>& running;
    int64_t& iterations;
    perf::statistics& stats;
    perf::stopwatch sw;
    phase phase_;

    pingpong_op(
        tcp_socket& c,
        tcp_socket& s,
        std::size_t message_size,
        std::atomic<bool>& r,
        int64_t& iters,
        perf::statistics& st )
        : client( c )
        , server( s )
        , send_buf( message_size, 'P' )
        , recv_buf( message_size )
        , running( r )
        , iterations( iters )
        , stats( st )
        , phase_( write_client )
    {
    }

    void start()
    {
        if( !running.load( std::memory_order_relaxed ) )
        {
            client.shutdown( tcp_socket::shutdown_send );
            return;
        }
        sw.reset();
        phase_ = write_client;
        do_step();
    }

    void do_step()
    {
        switch( phase_ )
        {
        case write_client:
            asio::async_write( client,
                asio::buffer( send_buf ),
                [this]( boost::system::error_code ec, std::size_t )
                {
                    if( ec ) return;
                    phase_ = read_server;
                    do_step();
                } );
            break;

        case read_server:
            asio::async_read( server,
                asio::buffer( recv_buf ),
                [this]( boost::system::error_code ec, std::size_t )
                {
                    if( ec ) return;
                    phase_ = write_server;
                    do_step();
                } );
            break;

        case write_server:
            asio::async_write( server,
                asio::buffer( recv_buf ),
                [this]( boost::system::error_code ec, std::size_t )
                {
                    if( ec ) return;
                    phase_ = read_client;
                    do_step();
                } );
            break;

        case read_client:
            asio::async_read( client,
                asio::buffer( recv_buf ),
                [this]( boost::system::error_code ec, std::size_t )
                {
                    if( ec ) return;
                    stats.add( sw.elapsed_us() );
                    ++iterations;
                    start();
                } );
            break;
        }
    }
};

bench::benchmark_result bench_pingpong_latency( std::size_t message_size, double duration_s )
{
    std::cout << "  Message size: " << message_size << " bytes\n";

    asio::io_context ioc;
    auto [client, server] = asio_bench::make_socket_pair( ioc );

    std::atomic<bool> running{ true };
    int64_t iterations = 0;
    perf::statistics latency_stats;

    pingpong_op op( client, server, message_size, running, iterations, latency_stats );

    op.start();

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

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    std::vector<perf::statistics> stats( num_pairs );
    std::vector<int64_t> iters( num_pairs, 0 );

    clients.reserve( num_pairs );
    servers.reserve( num_pairs );

    for( int i = 0; i < num_pairs; ++i )
    {
        auto [c, s] = asio_bench::make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    // Stable addresses needed for concurrent ops
    std::vector<std::unique_ptr<pingpong_op>> ops;
    ops.reserve( num_pairs );
    for( int p = 0; p < num_pairs; ++p )
    {
        ops.push_back( std::make_unique<pingpong_op>(
            clients[p], servers[p], message_size, running, iters[p], stats[p] ) );
        ops.back()->start();
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
        auto [c, s] = asio_bench::make_socket_pair( ioc );
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
        perf::print_header( "Ping-Pong Round-Trip Latency (Asio Callbacks)" );
        for( auto size : message_sizes )
            collector.add( bench_pingpong_latency( size, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Socket Pairs Latency (Asio Callbacks)" );
        collector.add( bench_concurrent_latency( 1, 64, duration_s ) );
        collector.add( bench_concurrent_latency( 4, 64, duration_s ) );
        collector.add( bench_concurrent_latency( 16, 64, duration_s ) );
    }
}

} // namespace asio_callback_bench
