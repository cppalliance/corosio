//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/connect.hpp>

#include <iostream>
#include <vector>

#include "../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// Create a connected socket pair using TCP loopback
std::pair<tcp::socket, tcp::socket> make_socket_pair(asio::io_context& ioc)
{
    tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));
    acceptor.set_option(tcp::acceptor::reuse_address(true));

    tcp::socket client(ioc);
    tcp::socket server(ioc);

    auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    server = acceptor.accept();

    // Disable Nagle's algorithm for low latency
    client.set_option(tcp::no_delay(true));
    server.set_option(tcp::no_delay(true));

    return {std::move(client), std::move(server)};
}

// Benchmark: Socket throughput with varying buffer sizes
void bench_throughput(std::size_t chunk_size, std::size_t total_bytes)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << (total_bytes / (1024 * 1024)) << " MB\n";

    asio::io_context ioc;
    auto [writer, reader] = make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::size_t total_written = 0;
    std::size_t total_read = 0;

    // Writer coroutine
    auto write_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while (total_written < total_bytes)
            {
                std::size_t to_write = std::min(chunk_size, total_bytes - total_written);
                auto n = co_await writer.async_write_some(
                    asio::buffer(write_buf.data(), to_write),
                    asio::use_awaitable);
                total_written += n;
            }
            writer.shutdown(tcp::socket::shutdown_send);
        }
        catch (std::exception const&) {}
    };

    // Reader coroutine
    auto read_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while (total_read < total_bytes)
            {
                auto n = co_await reader.async_read_some(
                    asio::buffer(read_buf.data(), read_buf.size()),
                    asio::use_awaitable);
                if (n == 0)
                    break;
                total_read += n;
            }
        }
        catch (std::exception const&) {}
    };

    bench::stopwatch sw;

    asio::co_spawn(ioc, write_task(), asio::detached);
    asio::co_spawn(ioc, read_task(), asio::detached);
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

    asio::io_context ioc;
    auto [sock1, sock2] = make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    // Socket 1 writes to socket 2
    auto write1_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while (written1 < total_bytes)
            {
                std::size_t to_write = std::min(chunk_size, total_bytes - written1);
                auto n = co_await sock1.async_write_some(
                    asio::buffer(buf1.data(), to_write),
                    asio::use_awaitable);
                written1 += n;
            }
            sock1.shutdown(tcp::socket::shutdown_send);
        }
        catch (std::exception const&) {}
    };

    // Socket 2 reads from socket 1
    auto read1_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            std::vector<char> rbuf(chunk_size);
            while (read1 < total_bytes)
            {
                auto n = co_await sock2.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()),
                    asio::use_awaitable);
                if (n == 0) break;
                read1 += n;
            }
        }
        catch (std::exception const&) {}
    };

    // Socket 2 writes to socket 1
    auto write2_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            while (written2 < total_bytes)
            {
                std::size_t to_write = std::min(chunk_size, total_bytes - written2);
                auto n = co_await sock2.async_write_some(
                    asio::buffer(buf2.data(), to_write),
                    asio::use_awaitable);
                written2 += n;
            }
            sock2.shutdown(tcp::socket::shutdown_send);
        }
        catch (std::exception const&) {}
    };

    // Socket 1 reads from socket 2
    auto read2_task = [&]() -> asio::awaitable<void>
    {
        try
        {
            std::vector<char> rbuf(chunk_size);
            while (read2 < total_bytes)
            {
                auto n = co_await sock1.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()),
                    asio::use_awaitable);
                if (n == 0) break;
                read2 += n;
            }
        }
        catch (std::exception const&) {}
    };

    bench::stopwatch sw;

    asio::co_spawn(ioc, write1_task(), asio::detached);
    asio::co_spawn(ioc, read1_task(), asio::detached);
    asio::co_spawn(ioc, write2_task(), asio::detached);
    asio::co_spawn(ioc, read2_task(), asio::detached);
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
    std::cout << "Boost.Asio Socket Throughput Benchmarks\n";
    std::cout << "=======================================\n";

    bench::print_header("Unidirectional Throughput (Asio)");

    // Variable buffer sizes
    std::vector<std::size_t> buffer_sizes = {1024, 4096, 16384, 65536};
    std::size_t transfer_size = 64 * 1024 * 1024; // 64 MB

    for (auto size : buffer_sizes)
        bench_throughput(size, transfer_size);

    bench::print_header("Bidirectional Throughput (Asio)");

    // Bidirectional with different buffer sizes
    for (auto size : buffer_sizes)
        bench_bidirectional_throughput(size, transfer_size / 2);

    std::cout << "\nBenchmarks complete.\n";
    return 0;
}
