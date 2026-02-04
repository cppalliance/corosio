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
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/buffer.hpp>

#include <cstring>
#include <iostream>
#include <vector>

#include "../common/benchmark.hpp"

namespace asio_bench {
namespace {

bench::benchmark_result bench_throughput( std::size_t chunk_size, std::size_t total_bytes )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << ( total_bytes / ( 1024 * 1024 ) ) << " MB\n";

    asio::io_context ioc;
    auto [writer, reader] = make_socket_pair( ioc );

    std::vector<char> write_buf( chunk_size, 'x' );
    std::vector<char> read_buf( chunk_size );

    std::size_t total_written = 0;
    std::size_t total_read = 0;

    auto write_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( total_written < total_bytes )
            {
                std::size_t to_write = ( std::min )( chunk_size, total_bytes - total_written );
                auto n = co_await writer.async_write_some(
                    asio::buffer( write_buf.data(), to_write ),
                    asio::use_awaitable );
                total_written += n;
            }
            writer.shutdown( tcp::socket::shutdown_send );
        }
        catch( std::exception const& ) {}
    };

    auto read_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( total_read < total_bytes )
            {
                auto n = co_await reader.async_read_some(
                    asio::buffer( read_buf.data(), read_buf.size() ),
                    asio::use_awaitable );
                if( n == 0 )
                    break;
                total_read += n;
            }
        }
        catch( std::exception const& ) {}
    };

    bench::stopwatch sw;

    asio::co_spawn( ioc, write_task(), asio::detached );
    asio::co_spawn( ioc, read_task(), asio::detached );
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double throughput = static_cast<double>( total_read ) / elapsed;

    std::cout << "    Written:    " << total_written << " bytes\n";
    std::cout << "    Read:       " << total_read << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_throughput( throughput ) << "\n\n";

    writer.close();
    reader.close();

    return bench::benchmark_result( "throughput_" + std::to_string( chunk_size ) )
        .add( "chunk_size", static_cast<double>( chunk_size ) )
        .add( "total_bytes", static_cast<double>( total_bytes ) )
        .add( "bytes_written", static_cast<double>( total_written ) )
        .add( "bytes_read", static_cast<double>( total_read ) )
        .add( "elapsed_s", elapsed )
        .add( "throughput_bytes_per_sec", throughput );
}

bench::benchmark_result bench_bidirectional_throughput( std::size_t chunk_size, std::size_t total_bytes )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << ( total_bytes / ( 1024 * 1024 ) ) << " MB each direction\n";

    asio::io_context ioc;
    auto [sock1, sock2] = make_socket_pair( ioc );

    std::vector<char> buf1( chunk_size, 'a' );
    std::vector<char> buf2( chunk_size, 'b' );

    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    auto write1_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( written1 < total_bytes )
            {
                std::size_t to_write = ( std::min )( chunk_size, total_bytes - written1 );
                auto n = co_await sock1.async_write_some(
                    asio::buffer( buf1.data(), to_write ),
                    asio::use_awaitable );
                written1 += n;
            }
            sock1.shutdown( tcp::socket::shutdown_send );
        }
        catch( std::exception const& ) {}
    };

    auto read1_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            std::vector<char> rbuf( chunk_size );
            while( read1 < total_bytes )
            {
                auto n = co_await sock2.async_read_some(
                    asio::buffer( rbuf.data(), rbuf.size() ),
                    asio::use_awaitable );
                if( n == 0 ) break;
                read1 += n;
            }
        }
        catch( std::exception const& ) {}
    };

    auto write2_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while( written2 < total_bytes )
            {
                std::size_t to_write = ( std::min )( chunk_size, total_bytes - written2 );
                auto n = co_await sock2.async_write_some(
                    asio::buffer( buf2.data(), to_write ),
                    asio::use_awaitable );
                written2 += n;
            }
            sock2.shutdown( tcp::socket::shutdown_send );
        }
        catch( std::exception const& ) {}
    };

    auto read2_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            std::vector<char> rbuf( chunk_size );
            while( read2 < total_bytes )
            {
                auto n = co_await sock1.async_read_some(
                    asio::buffer( rbuf.data(), rbuf.size() ),
                    asio::use_awaitable );
                if( n == 0 ) break;
                read2 += n;
            }
        }
        catch( std::exception const& ) {}
    };

    bench::stopwatch sw;

    asio::co_spawn( ioc, write1_task(), asio::detached );
    asio::co_spawn( ioc, read1_task(), asio::detached );
    asio::co_spawn( ioc, write2_task(), asio::detached );
    asio::co_spawn( ioc, read2_task(), asio::detached );
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    std::size_t total_transferred = read1 + read2;
    double throughput = static_cast<double>( total_transferred ) / elapsed;

    std::cout << "    Direction 1: " << read1 << " bytes\n";
    std::cout << "    Direction 2: " << read2 << " bytes\n";
    std::cout << "    Total:       " << total_transferred << " bytes\n";
    std::cout << "    Elapsed:     " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput:  " << bench::format_throughput( throughput )
              << " (combined)\n\n";

    sock1.close();
    sock2.close();

    return bench::benchmark_result( "bidirectional_" + std::to_string( chunk_size ) )
        .add( "chunk_size", static_cast<double>( chunk_size ) )
        .add( "total_bytes_per_direction", static_cast<double>( total_bytes ) )
        .add( "bytes_direction1", static_cast<double>( read1 ) )
        .add( "bytes_direction2", static_cast<double>( read2 ) )
        .add( "total_transferred", static_cast<double>( total_transferred ) )
        .add( "elapsed_s", elapsed )
        .add( "throughput_bytes_per_sec", throughput );
}

} // anonymous namespace

void run_socket_throughput_benchmarks(
    bench::result_collector& collector,
    char const* filter )
{
    std::cout << "\n>>> Socket Throughput Benchmarks (Asio) <<<\n";

    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    std::vector<std::size_t> buffer_sizes = { 1024, 4096, 16384, 65536 };
    std::size_t transfer_size = 64 * 1024 * 1024;

    if( run_all || std::strcmp( filter, "unidirectional" ) == 0 )
    {
        bench::print_header( "Unidirectional Throughput (Asio)" );
        for( auto size : buffer_sizes )
            collector.add( bench_throughput( size, transfer_size ) );
    }

    if( run_all || std::strcmp( filter, "bidirectional" ) == 0 )
    {
        bench::print_header( "Bidirectional Throughput (Asio)" );
        for( auto size : buffer_sizes )
            collector.add( bench_bidirectional_throughput( size, transfer_size / 2 ) );
    }
}

} // namespace asio_bench
