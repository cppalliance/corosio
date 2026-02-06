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
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace asio_callback_bench {
namespace {

struct write_op
{
    tcp::socket& sock;
    std::vector<char>& buf;
    std::size_t chunk_size;
    std::atomic<bool>& running;
    std::size_t& total_written;

    void start()
    {
        if( !running.load( std::memory_order_relaxed ) )
        {
            sock.shutdown( tcp::socket::shutdown_send );
            return;
        }
        sock.async_write_some(
            asio::buffer( buf.data(), chunk_size ),
            [this]( boost::system::error_code ec, std::size_t n )
            {
                if( ec )
                    return;
                total_written += n;
                start();
            } );
    }
};

struct read_op
{
    tcp::socket& sock;
    std::vector<char>& buf;
    std::size_t& total_read;

    void start()
    {
        sock.async_read_some(
            asio::buffer( buf.data(), buf.size() ),
            [this]( boost::system::error_code ec, std::size_t n )
            {
                if( ec || n == 0 )
                    return;
                total_read += n;
                start();
            } );
    }
};

bench::benchmark_result bench_throughput( std::size_t chunk_size, double duration_s )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes\n";

    asio::io_context ioc;
    auto [writer, reader] = asio_bench::make_socket_pair( ioc );

    std::vector<char> write_buf( chunk_size, 'x' );
    std::vector<char> read_buf( chunk_size );

    std::atomic<bool> running{ true };
    std::size_t total_written = 0;
    std::size_t total_read = 0;

    write_op wop{ writer, write_buf, chunk_size, running, total_written };
    read_op rop{ reader, read_buf, total_read };

    perf::stopwatch sw;

    wop.start();
    rop.start();

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
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

bench::benchmark_result bench_bidirectional_throughput( std::size_t chunk_size, double duration_s )
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, bidirectional\n";

    asio::io_context ioc;
    auto [sock1, sock2] = asio_bench::make_socket_pair( ioc );

    std::vector<char> buf1( chunk_size, 'a' );
    std::vector<char> buf2( chunk_size, 'b' );
    std::vector<char> rbuf1( chunk_size );
    std::vector<char> rbuf2( chunk_size );

    std::atomic<bool> running{ true };
    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    // sock1 writes, sock2 reads (direction 1)
    write_op wop1{ sock1, buf1, chunk_size, running, written1 };
    read_op rop1{ sock2, rbuf1, read1 };

    // sock2 writes, sock1 reads (direction 2)
    write_op wop2{ sock2, buf2, chunk_size, running, written2 };
    read_op rop2{ sock1, rbuf2, read2 };

    perf::stopwatch sw;

    wop1.start();
    rop1.start();
    wop2.start();
    rop2.start();

    std::thread timer( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
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
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    // Warm up
    {
        asio::io_context ioc;
        auto [w, r] = asio_bench::make_socket_pair( ioc );
        std::vector<char> buf( 4096, 'w' );
        asio::write( w, asio::buffer( buf ) );
        asio::read( r, asio::buffer( buf ) );
        w.close();
        r.close();
    }

    std::vector<std::size_t> buffer_sizes = { 1024, 4096, 16384, 65536 };

    if( run_all || std::strcmp( filter, "unidirectional" ) == 0 )
    {
        perf::print_header( "Unidirectional Throughput (Asio Callbacks)" );
        for( auto size : buffer_sizes )
            collector.add( bench_throughput( size, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "bidirectional" ) == 0 )
    {
        perf::print_header( "Bidirectional Throughput (Asio Callbacks)" );
        for( auto size : buffer_sizes )
            collector.add( bench_bidirectional_throughput( size, duration_s ) );
    }
}

} // namespace asio_callback_bench
