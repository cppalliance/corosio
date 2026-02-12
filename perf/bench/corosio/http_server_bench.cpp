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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/read_until.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"
#include "../common/http_protocol.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

capy::task<> server_task(
    corosio::tcp_socket& sock,
    int64_t& completed_requests )
{
    std::string buf;

    for( ;; )
    {
        auto [ec, n] = co_await capy::read_until(
            sock, capy::dynamic_buffer( buf ), "\r\n\r\n" );
        if( ec )
            co_return;

        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer( bench::http::small_response, bench::http::small_response_size ) );
        if( wec )
            co_return;

        ++completed_requests;
        buf.erase( 0, n );
    }
}

capy::task<> client_task(
    corosio::tcp_socket& sock,
    std::atomic<bool>& running,
    int64_t& request_count,
    perf::statistics& latency_stats )
{
    std::string buf;

    while( running.load( std::memory_order_relaxed ) )
    {
        perf::stopwatch sw;

        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer( bench::http::small_request, bench::http::small_request_size ) );
        if( wec )
            co_return;

        auto [ec, header_end] = co_await capy::read_until(
            sock, capy::dynamic_buffer( buf ), "\r\n\r\n" );
        if( ec )
            co_return;

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
            auto [rec, rn] = co_await capy::read(
                sock, capy::mutable_buffer( buf.data() + old_size, need ) );
            if( rec )
                co_return;
        }

        double latency_us = sw.elapsed_us();
        latency_stats.add( latency_us );
        ++request_count;

        buf.erase( 0, total_size );
    }

    sock.shutdown( corosio::tcp_socket::shutdown_send );
}

bench::benchmark_result bench_single_connection(
    perf::context_factory factory, double duration_s )
{
    perf::print_header( "Single Connection (Corosio)" );

    auto ioc = factory( 1 );
    auto [client, server] = corosio::test::make_socket_pair( *ioc );

    client.set_no_delay( true );
    server.set_no_delay( true );

    std::atomic<bool> running{ true };
    int64_t completed_requests = 0;
    int64_t request_count = 0;
    perf::statistics latency_stats;

    perf::stopwatch total_sw;

    capy::run_async( ioc->get_executor() )(
        server_task( server, completed_requests ) );
    capy::run_async( ioc->get_executor() )(
        client_task( client, running, request_count, latency_stats ) );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
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

bench::benchmark_result bench_concurrent_connections(
    perf::context_factory factory, int num_connections, double duration_s )
{
    std::cout << "  Connections: " << num_connections << "\n";

    auto ioc = factory( 1 );

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    std::vector<int64_t> server_completed( num_connections, 0 );
    std::vector<int64_t> client_counts( num_connections, 0 );
    std::vector<perf::statistics> stats( num_connections );

    clients.reserve( num_connections );
    servers.reserve( num_connections );

    for( int i = 0; i < num_connections; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    perf::stopwatch total_sw;

    for( int i = 0; i < num_connections; ++i )
    {
        capy::run_async( ioc->get_executor() )(
            server_task( servers[i], server_completed[i] ) );
        capy::run_async( ioc->get_executor() )(
            client_task( clients[i], running, client_counts[i], stats[i] ) );
    }

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
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
    perf::context_factory factory, int num_threads, int num_connections, double duration_s )
{
    std::cout << "  Threads: " << num_threads
              << ", Connections: " << num_connections << "\n";

    auto ioc = factory( num_threads );

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    std::vector<int64_t> server_completed( num_connections, 0 );
    std::vector<int64_t> client_counts( num_connections, 0 );
    std::vector<perf::statistics> stats( num_connections );

    clients.reserve( num_connections );
    servers.reserve( num_connections );

    for( int i = 0; i < num_connections; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    std::atomic<bool> running{ true };

    for( int i = 0; i < num_connections; ++i )
    {
        capy::run_async( ioc->get_executor() )(
            server_task( servers[i], server_completed[i] ) );
        capy::run_async( ioc->get_executor() )(
            client_task( clients[i], running, client_counts[i], stats[i] ) );
    }

    perf::stopwatch total_sw;

    std::vector<std::thread> threads;
    threads.reserve( num_threads - 1 );
    for( int i = 1; i < num_threads; ++i )
        threads.emplace_back( [&ioc] { ioc->run(); } );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();

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
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        auto ioc = factory( 1 );
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        char buf[256] = {};
        auto task = [&]() -> capy::task<>
        {
            for( int i = 0; i < 10; ++i )
            {
                (void)co_await capy::write(
                    c, capy::const_buffer( bench::http::small_request, bench::http::small_request_size ) );
                (void)co_await s.read_some(
                    capy::mutable_buffer( buf, bench::http::small_request_size ) );
                (void)co_await capy::write(
                    s, capy::const_buffer( bench::http::small_response, bench::http::small_response_size ) );
                (void)co_await c.read_some(
                    capy::mutable_buffer( buf, bench::http::small_response_size ) );
            }
        };
        capy::run_async( ioc->get_executor() )( task() );
        ioc->run();
        c.close();
        s.close();
    }

    if( run_all || std::strcmp( filter, "single_conn" ) == 0 )
        collector.add( bench_single_connection( factory, duration_s ) );

    if( run_all || std::strcmp( filter, "concurrent" ) == 0 )
    {
        perf::print_header( "Concurrent Connections (Corosio)" );
        collector.add( bench_concurrent_connections( factory, 1, duration_s ) );
        collector.add( bench_concurrent_connections( factory, 4, duration_s ) );
        collector.add( bench_concurrent_connections( factory, 16, duration_s ) );
        collector.add( bench_concurrent_connections( factory, 32, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "multithread" ) == 0 )
    {
        perf::print_header( "Multi-threaded (Corosio)" );
        collector.add( bench_multithread( factory, 1, 32, duration_s ) );
        collector.add( bench_multithread( factory, 2, 32, duration_s ) );
        collector.add( bench_multithread( factory, 4, 32, duration_s ) );
        collector.add( bench_multithread( factory, 8, 32, duration_s ) );
        collector.add( bench_multithread( factory, 16, 32, duration_s ) );
    }
}

} // namespace corosio_bench
