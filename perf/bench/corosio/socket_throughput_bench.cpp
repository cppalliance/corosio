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
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#if BOOST_COROSIO_HAS_IOCP
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

inline void set_nodelay( corosio::tcp_socket& s )
{
    int flag = 1;
#if BOOST_COROSIO_HAS_IOCP
    ::setsockopt( static_cast<SOCKET>( s.native_handle() ), IPPROTO_TCP, TCP_NODELAY,
                  reinterpret_cast<char const*>( &flag ), sizeof( flag ) );
#else
    ::setsockopt( s.native_handle(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof( flag ) );
#endif
}

bench::benchmark_result bench_throughput(
    perf::context_factory factory, std::size_t chunk_size, double duration_s )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes\n";

    auto ioc = factory();
    auto [writer, reader] = corosio::test::make_socket_pair( *ioc );

    set_nodelay( writer );
    set_nodelay( reader );

    std::vector<char> write_buf( chunk_size, 'x' );
    std::vector<char> read_buf( chunk_size );

    std::atomic<bool> running{ true };
    std::size_t total_written = 0;
    std::size_t total_read = 0;

    auto write_task = [&]() -> capy::task<>
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer( write_buf.data(), chunk_size ) );
            if( ec )
                break;
            total_written += n;
        }
        writer.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read_task = [&]() -> capy::task<>
    {
        for( ;; )
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer( read_buf.data(), read_buf.size() ) );
            if( ec || n == 0 )
                break;
            total_read += n;
        }
    };

    perf::stopwatch sw;

    capy::run_async( ioc->get_executor() )( write_task() );
    capy::run_async( ioc->get_executor() )( read_task() );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
    timer.join();

    double elapsed = sw.elapsed_seconds();
    double throughput = static_cast<double>( total_read ) / elapsed;

    std::cout << "    Written:    " << total_written << " bytes\n";
    std::cout << "    Read:       " << total_read << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_throughput( throughput ) << "\n\n";

    writer.close();
    reader.close();

    return bench::benchmark_result( "throughput_" + std::to_string( chunk_size ) )
        .add( "chunk_size", static_cast<double>( chunk_size ) )
        .add( "bytes_written", static_cast<double>( total_written ) )
        .add( "bytes_read", static_cast<double>( total_read ) )
        .add( "elapsed_s", elapsed )
        .add( "throughput_bytes_per_sec", throughput );
}

bench::benchmark_result bench_bidirectional_throughput(
    perf::context_factory factory, std::size_t chunk_size, double duration_s )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, bidirectional\n";

    auto ioc = factory();
    auto [sock1, sock2] = corosio::test::make_socket_pair( *ioc );

    set_nodelay( sock1 );
    set_nodelay( sock2 );

    std::vector<char> buf1( chunk_size, 'a' );
    std::vector<char> buf2( chunk_size, 'b' );

    std::atomic<bool> running{ true };
    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    auto write1_task = [&]() -> capy::task<>
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer( buf1.data(), chunk_size ) );
            if( ec ) break;
            written1 += n;
        }
        sock1.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read1_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf( chunk_size );
        for( ;; )
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer( rbuf.data(), rbuf.size() ) );
            if( ec || n == 0 ) break;
            read1 += n;
        }
    };

    auto write2_task = [&]() -> capy::task<>
    {
        while( running.load( std::memory_order_relaxed ) )
        {
            auto [ec, n] = co_await sock2.write_some(
                capy::const_buffer( buf2.data(), chunk_size ) );
            if( ec ) break;
            written2 += n;
        }
        sock2.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read2_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf( chunk_size );
        for( ;; )
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer( rbuf.data(), rbuf.size() ) );
            if( ec || n == 0 ) break;
            read2 += n;
        }
    };

    perf::stopwatch sw;

    capy::run_async( ioc->get_executor() )( write1_task() );
    capy::run_async( ioc->get_executor() )( read1_task() );
    capy::run_async( ioc->get_executor() )( write2_task() );
    capy::run_async( ioc->get_executor() )( read2_task() );

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
    timer.join();

    double elapsed = sw.elapsed_seconds();
    std::size_t total_transferred = read1 + read2;
    double throughput = static_cast<double>( total_transferred ) / elapsed;

    std::cout << "    Direction 1: " << read1 << " bytes\n";
    std::cout << "    Direction 2: " << read2 << " bytes\n";
    std::cout << "    Total:       " << total_transferred << " bytes\n";
    std::cout << "    Elapsed:     " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput:  " << perf::format_throughput( throughput )
              << " (combined)\n\n";

    sock1.close();
    sock2.close();

    return bench::benchmark_result( "bidirectional_" + std::to_string( chunk_size ) )
        .add( "chunk_size", static_cast<double>( chunk_size ) )
        .add( "bytes_direction1", static_cast<double>( read1 ) )
        .add( "bytes_direction2", static_cast<double>( read2 ) )
        .add( "total_transferred", static_cast<double>( total_transferred ) )
        .add( "elapsed_s", elapsed )
        .add( "throughput_bytes_per_sec", throughput );
}

} // anonymous namespace

void run_socket_throughput_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        auto ioc = factory();
        auto [w, r] = corosio::test::make_socket_pair( *ioc );
        std::vector<char> buf( 4096, 'w' );
        auto task = [&]() -> capy::task<>
        {
            (void)co_await w.write_some( capy::const_buffer( buf.data(), buf.size() ) );
            (void)co_await r.read_some( capy::mutable_buffer( buf.data(), buf.size() ) );
        };
        capy::run_async( ioc->get_executor() )( task() );
        ioc->run();
        w.close();
        r.close();
    }

    std::vector<std::size_t> buffer_sizes = { 1024, 4096, 16384, 65536 };

    if( run_all || std::strcmp( filter, "unidirectional" ) == 0 )
    {
        perf::print_header( "Unidirectional Throughput (Corosio)" );
        for( auto size : buffer_sizes )
            collector.add( bench_throughput( factory, size, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "bidirectional" ) == 0 )
    {
        perf::print_header( "Bidirectional Throughput (Corosio)" );
        for( auto size : buffer_sizes )
            collector.add( bench_bidirectional_throughput( factory, size, duration_s ) );
    }
}

} // namespace corosio_bench
