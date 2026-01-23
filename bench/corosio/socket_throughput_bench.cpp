//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
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

// Helper to set TCP_NODELAY on a socket for low latency
inline void set_nodelay(corosio::socket& s)
{
    int flag = 1;
#ifdef _WIN32
    ::setsockopt(static_cast<SOCKET>(s.native_handle()), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    ::setsockopt(s.native_handle(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
}

// Benchmark: Socket throughput with varying buffer sizes
void bench_throughput(std::size_t chunk_size, std::size_t total_bytes)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << (total_bytes / (1024 * 1024)) << " MB\n";

    corosio::io_context ioc;
    auto [writer, reader] = corosio::test::make_socket_pair(ioc);

    // Disable Nagle's algorithm for fair comparison with Asio
    set_nodelay(writer);
    set_nodelay(reader);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::size_t total_written = 0;
    std::size_t total_read = 0;
    bool writer_done = false;

    // Writer coroutine
    auto write_task = [&]() -> capy::task<>
    {
        while (total_written < total_bytes)
        {
            std::size_t to_write = std::min(chunk_size, total_bytes - total_written);
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), to_write));
            if (ec)
            {
                std::cerr << "    Write error: " << ec.message() << "\n";
                break;
            }
            total_written += n;
        }
        writer_done = true;
        writer.shutdown(corosio::socket::shutdown_send);
    };

    // Reader coroutine
    auto read_task = [&]() -> capy::task<>
    {
        while (total_read < total_bytes)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec)
            {
                if (writer_done && total_read >= total_bytes)
                    break;
                std::cerr << "    Read error: " << ec.message() << "\n";
                break;
            }
            if (n == 0)
                break;
            total_read += n;
        }
    };

    bench::stopwatch sw;

    capy::run_async(ioc.get_executor())(write_task());
    capy::run_async(ioc.get_executor())(read_task());
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double throughput = static_cast<double>(total_read) / elapsed;

    std::cout << "    Written:    " << total_written << " bytes\n";
    std::cout << "    Read:       " << total_read << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_throughput(throughput) << "\n\n";

    writer.close();
    reader.close();
}

// Benchmark: Bidirectional throughput
void bench_bidirectional_throughput(std::size_t chunk_size, std::size_t total_bytes)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << (total_bytes / (1024 * 1024)) << " MB each direction\n";

    corosio::io_context ioc;
    auto [sock1, sock2] = corosio::test::make_socket_pair(ioc);

    // Disable Nagle's algorithm for fair comparison with Asio
    set_nodelay(sock1);
    set_nodelay(sock2);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    // Socket 1 writes to socket 2
    auto write1_task = [&]() -> capy::task<>
    {
        while (written1 < total_bytes)
        {
            std::size_t to_write = std::min(chunk_size, total_bytes - written1);
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), to_write));
            if (ec) break;
            written1 += n;
        }
        sock1.shutdown(corosio::socket::shutdown_send);
    };

    // Socket 2 reads from socket 1
    auto read1_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf(chunk_size);
        while (read1 < total_bytes)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0) break;
            read1 += n;
        }
    };

    // Socket 2 writes to socket 1
    auto write2_task = [&]() -> capy::task<>
    {
        while (written2 < total_bytes)
        {
            std::size_t to_write = std::min(chunk_size, total_bytes - written2);
            auto [ec, n] = co_await sock2.write_some(
                capy::const_buffer(buf2.data(), to_write));
            if (ec) break;
            written2 += n;
        }
        sock2.shutdown(corosio::socket::shutdown_send);
    };

    // Socket 1 reads from socket 2
    auto read2_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf(chunk_size);
        while (read2 < total_bytes)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0) break;
            read2 += n;
        }
    };

    bench::stopwatch sw;

    capy::run_async(ioc.get_executor())(write1_task());
    capy::run_async(ioc.get_executor())(read1_task());
    capy::run_async(ioc.get_executor())(write2_task());
    capy::run_async(ioc.get_executor())(read2_task());
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    std::size_t total_transferred = read1 + read2;
    double throughput = static_cast<double>(total_transferred) / elapsed;

    std::cout << "    Direction 1: " << read1 << " bytes\n";
    std::cout << "    Direction 2: " << read2 << " bytes\n";
    std::cout << "    Total:       " << total_transferred << " bytes\n";
    std::cout << "    Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput:  " << bench::format_throughput(throughput)
              << " (combined)\n\n";

    sock1.close();
    sock2.close();
}

int main()
{
    std::cout << "Boost.Corosio Socket Throughput Benchmarks\n";
    std::cout << "==========================================\n";

    bench::print_header("Unidirectional Throughput");

    // Variable buffer sizes
    std::vector<std::size_t> buffer_sizes = {1024, 4096, 16384, 65536};
    std::size_t transfer_size = 64 * 1024 * 1024; // 64 MB

    for (auto size : buffer_sizes)
        bench_throughput(size, transfer_size);

    bench::print_header("Bidirectional Throughput");

    // Bidirectional with different buffer sizes
    for (auto size : buffer_sizes)
        bench_bidirectional_throughput(size, transfer_size / 2);

    std::cout << "\nBenchmarks complete.\n";
    return 0;
}
