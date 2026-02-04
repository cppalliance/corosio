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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <cstring>
#include <iostream>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

capy::task<> pingpong_task(
    corosio::tcp_socket& client,
    corosio::tcp_socket& server,
    std::size_t message_size,
    int iterations,
    bench::statistics& stats )
{
    std::vector<char> send_buf( message_size, 'P' );
    std::vector<char> recv_buf( message_size );

    for( int i = 0; i < iterations; ++i )
    {
        bench::stopwatch sw;

        auto [ec1, n1] = co_await capy::write(
            client, capy::const_buffer( send_buf.data(), send_buf.size() ) );
        if( ec1 )
        {
            std::cerr << "    Write error: " << ec1.message() << "\n";
            co_return;
        }

        auto [ec2, n2] = co_await capy::read(
            server, capy::mutable_buffer( recv_buf.data(), recv_buf.size() ) );
        if( ec2 )
        {
            std::cerr << "    Server read error: " << ec2.message() << "\n";
            co_return;
        }

        auto [ec3, n3] = co_await capy::write(
            server, capy::const_buffer( recv_buf.data(), n2 ) );
        if( ec3 )
        {
            std::cerr << "    Server write error: " << ec3.message() << "\n";
            co_return;
        }

        auto [ec4, n4] = co_await capy::read(
            client, capy::mutable_buffer( recv_buf.data(), recv_buf.size() ) );
        if( ec4 )
        {
            std::cerr << "    Client read error: " << ec4.message() << "\n";
            co_return;
        }

        double rtt_us = sw.elapsed_us();
        stats.add( rtt_us );
    }
}

template<typename Context>
bench::benchmark_result bench_pingpong_latency( std::size_t message_size, int iterations )
{
    std::cout << "  Message size: " << message_size << " bytes, ";
    std::cout << "Iterations: " << iterations << "\n";

    Context ioc;
    auto [client, server] = corosio::test::make_socket_pair( ioc );

    client.set_no_delay( true );
    server.set_no_delay( true );

    bench::statistics latency_stats;

    capy::run_async( ioc.get_executor() )(
        pingpong_task( client, server, message_size, iterations, latency_stats ) );
    ioc.run();

    bench::print_latency_stats( latency_stats, "Round-trip latency" );
    std::cout << "\n";

    client.close();
    server.close();

    return bench::benchmark_result( "pingpong_" + std::to_string( message_size ) )
        .add( "message_size", static_cast<double>( message_size ) )
        .add( "iterations", iterations )
        .add_latency_stats( "rtt", latency_stats );
}

template<typename Context>
bench::benchmark_result bench_concurrent_latency( int num_pairs, std::size_t message_size, int iterations )
{
    std::cout << "  Concurrent pairs: " << num_pairs << ", ";
    std::cout << "Message size: " << message_size << " bytes, ";
    std::cout << "Iterations: " << iterations << "\n";

    Context ioc;

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    std::vector<bench::statistics> stats( num_pairs );

    clients.reserve( num_pairs );
    servers.reserve( num_pairs );

    for( int i = 0; i < num_pairs; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int p = 0; p < num_pairs; ++p )
    {
        capy::run_async( ioc.get_executor() )(
            pingpong_task( clients[p], servers[p], message_size, iterations, stats[p] ) );
    }

    ioc.run();

    std::cout << "  Per-pair results:\n";
    for( int i = 0; i < num_pairs && i < 3; ++i )
    {
        std::cout << "    Pair " << i << ": mean="
                  << bench::format_latency( stats[i].mean() )
                  << ", p99=" << bench::format_latency( stats[i].p99() )
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
              << bench::format_latency( total_mean / num_pairs ) << "\n";
    std::cout << "  Average p99 latency:  "
              << bench::format_latency( total_p99 / num_pairs ) << "\n\n";

    for( auto& c : clients )
        c.close();
    for( auto& s : servers )
        s.close();

    return bench::benchmark_result( "concurrent_" + std::to_string( num_pairs ) + "_pairs" )
        .add( "num_pairs", num_pairs )
        .add( "message_size", static_cast<double>( message_size ) )
        .add( "iterations", iterations )
        .add( "avg_mean_latency_us", total_mean / num_pairs )
        .add( "avg_p99_latency_us", total_p99 / num_pairs );
}

} // anonymous namespace

template<typename Context>
void run_socket_latency_benchmarks(
    bench::result_collector& collector,
    char const* filter )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        Context ioc;
        auto [c, s] = corosio::test::make_socket_pair( ioc );
        char buf[64] = {};
        auto task = [&]() -> capy::task<>
        {
            for( int i = 0; i < 100; ++i )
            {
                (void)co_await c.write_some( capy::const_buffer( buf, sizeof( buf ) ) );
                (void)co_await s.read_some( capy::mutable_buffer( buf, sizeof( buf ) ) );
            }
        };
        capy::run_async( ioc.get_executor() )( task() );
        ioc.run();
        c.close();
        s.close();
    }

    std::vector<std::size_t> message_sizes = { 1, 64, 1024 };
    int iterations = 1000000;

    if( run_all || std::strcmp( filter, "pingpong" ) == 0 )
    {
        bench::print_header( "Ping-Pong Round-Trip Latency" );
        for( auto size : message_sizes )
            collector.add( bench_pingpong_latency<Context>( size, iterations ) );
    }

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        bench::print_header( "Concurrent Socket Pairs Latency" );
        collector.add( bench_concurrent_latency<Context>( 1, 64, 1000000 ) );
        collector.add( bench_concurrent_latency<Context>( 4, 64, 500000 ) );
        collector.add( bench_concurrent_latency<Context>( 16, 64, 250000 ) );
    }
}

// Explicit instantiations
#if BOOST_COROSIO_HAS_EPOLL
template void run_socket_latency_benchmarks<corosio::epoll_context>(
    bench::result_collector&, char const* );
#endif
#if BOOST_COROSIO_HAS_SELECT
template void run_socket_latency_benchmarks<corosio::select_context>(
    bench::result_collector&, char const* );
#endif
#if BOOST_COROSIO_HAS_IOCP
template void run_socket_latency_benchmarks<corosio::iocp_context>(
    bench::result_collector&, char const* );
#endif

} // namespace corosio_bench
