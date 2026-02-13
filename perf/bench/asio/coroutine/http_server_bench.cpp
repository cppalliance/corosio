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
#include <boost/asio/deferred.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"
#include "../../common/http_protocol.hpp"

namespace asio_bench {
namespace {

// Server: loop until read error (EOF from client shutdown)
asio::awaitable<void, executor_type> server_task(
    tcp_socket& sock,
    int64_t& completed_requests )
{
    std::string buf;

    try
    {
        for( ;; )
        {
            std::size_t n = co_await asio::async_read_until(
                sock,
                asio::dynamic_buffer( buf ),
                "\r\n\r\n",
                asio::deferred );

            co_await asio::async_write(
                sock,
                asio::buffer( bench::http::small_response, bench::http::small_response_size ),
                asio::deferred );

            ++completed_requests;
            buf.erase( 0, n );
        }
    }
    catch( std::exception const& ) {}
}

// Client: loop while running, then shutdown
asio::awaitable<void, executor_type> client_task(
    tcp_socket& sock,
    std::atomic<bool>& running,
    int64_t& request_count,
    perf::statistics& latency_stats )
{
    std::string buf;

    try
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            co_await asio::async_write(
                sock,
                asio::buffer( bench::http::small_request, bench::http::small_request_size ),
                asio::deferred );

            std::size_t header_end = co_await asio::async_read_until(
                sock,
                asio::dynamic_buffer( buf ),
                "\r\n\r\n",
                asio::deferred );

            std::string_view headers( buf.data(), header_end );
            std::size_t content_length = 0;
            auto pos = headers.find( "Content-Length: " );
            if( pos != std::string_view::npos )
            {
                pos += 16;
                while( pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9' )
                {
                    content_length = content_length * 10 + ( headers[pos] - '0' );
                    ++pos;
                }
            }

            std::size_t total_size = header_end + content_length;
            if( buf.size() < total_size )
            {
                std::size_t need = total_size - buf.size();
                std::size_t old_size = buf.size();
                buf.resize( total_size );
                co_await asio::async_read(
                    sock,
                    asio::buffer( buf.data() + old_size, need ),
                    asio::deferred );
            }

            double latency_us = sw.elapsed_us();
            latency_stats.add( latency_us );
            ++request_count;

            buf.erase( 0, total_size );
        }

        sock.shutdown( tcp_socket::shutdown_send );
    }
    catch( std::exception const& ) {}
}

bench::benchmark_result bench_single_connection( double duration_s )
{
    perf::print_header( "Single Connection (Asio Coroutines)" );

    asio::io_context ioc;
    auto [client, server] = make_socket_pair( ioc );

    std::atomic<bool> running{ true };
    int64_t completed_requests = 0;
    int64_t request_count = 0;
    perf::statistics latency_stats;

    perf::stopwatch total_sw;

    asio::co_spawn( ioc,
        server_task( server, completed_requests ),
        asio::detached );
    asio::co_spawn( ioc,
        client_task( client, running, request_count, latency_stats ),
        asio::detached );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    timer.join();

    double elapsed = total_sw.elapsed_seconds();
    double requests_per_sec = static_cast<double>( request_count ) / elapsed;

    std::cout << "    Completed: " << request_count << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( requests_per_sec ) << "\n";
    perf::print_latency_stats( latency_stats, "Request latency" );
    std::cout << "\n";

    client.close();
    server.close();

    return bench::benchmark_result( "single_conn" )
        .add( "num_connections", 1 )
        .add( "total_requests", static_cast<double>( request_count ) )
        .add( "requests_per_sec", requests_per_sec )
        .add_latency_stats( "request_latency", latency_stats );
}

bench::benchmark_result bench_concurrent_connections( int num_connections, double duration_s )
{
    std::cout << "  Connections: " << num_connections << "\n";

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    std::vector<int64_t> server_completed( num_connections, 0 );
    std::vector<int64_t> client_counts( num_connections, 0 );
    std::vector<perf::statistics> stats( num_connections );

    clients.reserve( num_connections );
    servers.reserve( num_connections );

    for( int i = 0; i < num_connections; ++i )
    {
        auto [c, s] = make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    perf::stopwatch total_sw;

    for( int i = 0; i < num_connections; ++i )
    {
        asio::co_spawn( ioc,
            server_task( servers[i], server_completed[i] ),
            asio::detached );
        asio::co_spawn( ioc,
            client_task( clients[i], running, client_counts[i], stats[i] ),
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

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_requests = 0;
    for( auto c : client_counts )
        total_requests += c;

    double requests_per_sec = static_cast<double>( total_requests ) / elapsed;

    double total_mean = 0;
    double total_p99 = 0;
    for( auto& s : stats )
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Completed: " << total_requests << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( requests_per_sec ) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency( total_mean / num_connections ) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency( total_p99 / num_connections ) << "\n\n";

    for( auto& c : clients )
        c.close();
    for( auto& s : servers )
        s.close();

    return bench::benchmark_result( "concurrent_" + std::to_string( num_connections ) )
        .add( "num_connections", num_connections )
        .add( "total_requests", static_cast<double>( total_requests ) )
        .add( "requests_per_sec", requests_per_sec )
        .add( "avg_mean_latency_us", total_mean / num_connections )
        .add( "avg_p99_latency_us", total_p99 / num_connections );
}

bench::benchmark_result bench_multithread(
    int num_threads, int num_connections, double duration_s )
{
    std::cout << "  Threads: " << num_threads
              << ", Connections: " << num_connections << "\n";

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    std::vector<int64_t> server_completed( num_connections, 0 );
    std::vector<int64_t> client_counts( num_connections, 0 );
    std::vector<perf::statistics> stats( num_connections );

    clients.reserve( num_connections );
    servers.reserve( num_connections );

    for( int i = 0; i < num_connections; ++i )
    {
        auto [c, s] = make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    for( int i = 0; i < num_connections; ++i )
    {
        asio::co_spawn( ioc,
            server_task( servers[i], server_completed[i] ),
            asio::detached );
        asio::co_spawn( ioc,
            client_task( clients[i], running, client_counts[i], stats[i] ),
            asio::detached );
    }

    perf::stopwatch total_sw;

    std::vector<std::thread> threads;
    threads.reserve( num_threads - 1 );
    for( int i = 1; i < num_threads; ++i )
        threads.emplace_back( [&ioc] { ioc.run(); } );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();

    timer.join();
    for( auto& t : threads )
        t.join();

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_requests = 0;
    for( auto c : client_counts )
        total_requests += c;

    double requests_per_sec = static_cast<double>( total_requests ) / elapsed;

    double total_mean = 0;
    double total_p99 = 0;
    for( auto& s : stats )
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Completed: " << total_requests << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( requests_per_sec ) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency( total_mean / num_connections ) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency( total_p99 / num_connections ) << "\n\n";

    for( auto& c : clients )
        c.close();
    for( auto& s : servers )
        s.close();

    return bench::benchmark_result( "multithread_" + std::to_string( num_threads ) + "t" )
        .add( "num_threads", num_threads )
        .add( "num_connections", num_connections )
        .add( "total_requests", static_cast<double>( total_requests ) )
        .add( "requests_per_sec", requests_per_sec )
        .add( "avg_mean_latency_us", total_mean / num_connections )
        .add( "avg_p99_latency_us", total_p99 / num_connections );
}

} // anonymous namespace

void run_http_server_benchmarks(
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        asio::io_context ioc;
        auto [c, s] = make_socket_pair( ioc );
        char buf[256] = {};
        for( int i = 0; i < 10; ++i )
        {
            asio::write( c, asio::buffer( bench::http::small_request, bench::http::small_request_size ) );
            asio::read( s, asio::buffer( buf, bench::http::small_request_size ) );
            asio::write( s, asio::buffer( bench::http::small_response, bench::http::small_response_size ) );
            asio::read( c, asio::buffer( buf, bench::http::small_response_size ) );
        }
        c.close();
        s.close();
    }

    if( run_all || std::strcmp( filter, "single_conn" ) == 0 )
        collector.add( bench_single_connection( duration_s ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Connections (Asio Coroutines)" );
        collector.add( bench_concurrent_connections( 1, duration_s ) );
        collector.add( bench_concurrent_connections( 4, duration_s ) );
        collector.add( bench_concurrent_connections( 16, duration_s ) );
        collector.add( bench_concurrent_connections( 32, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "multithread" ) == 0 )
    {
        perf::print_header( "Multi-threaded (Asio Coroutines)" );
        collector.add( bench_multithread( 1, 32, duration_s ) );
        collector.add( bench_multithread( 2, 32, duration_s ) );
        collector.add( bench_multithread( 4, 32, duration_s ) );
        collector.add( bench_multithread( 8, 32, duration_s ) );
        collector.add( bench_multithread( 16, 32, duration_s ) );
    }
}

} // namespace asio_bench
