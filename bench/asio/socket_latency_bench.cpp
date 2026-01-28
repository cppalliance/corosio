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
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

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

// Ping-pong coroutine task
asio::awaitable<void> pingpong_task(
    tcp::socket& client,
    tcp::socket& server,
    std::size_t message_size,
    int iterations,
    bench::statistics& stats)
{
    std::vector<char> send_buf(message_size, 'P');
    std::vector<char> recv_buf(message_size);

    try
    {
        for (int i = 0; i < iterations; ++i)
        {
            bench::stopwatch sw;

            // Client sends ping
            co_await asio::async_write(
                client,
                asio::buffer(send_buf.data(), send_buf.size()),
                asio::use_awaitable);

            // Server receives ping
            co_await asio::async_read(
                server,
                asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::use_awaitable);

            // Server sends pong
            co_await asio::async_write(
                server,
                asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::use_awaitable);

            // Client receives pong
            co_await asio::async_read(
                client,
                asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::use_awaitable);

            double rtt_us = sw.elapsed_us();
            stats.add(rtt_us);
        }
    }
    catch (std::exception const&) {}
}

// Benchmark: Ping-pong latency measurement
void bench_pingpong_latency(std::size_t message_size, int iterations)
{
    std::cout << "  Message size: " << message_size << " bytes, ";
    std::cout << "Iterations: " << iterations << "\n";

    asio::io_context ioc;
    auto [client, server] = make_socket_pair(ioc);

    bench::statistics latency_stats;

    asio::co_spawn(ioc,
        pingpong_task(client, server, message_size, iterations, latency_stats),
        asio::detached);
    ioc.run();

    bench::print_latency_stats(latency_stats, "Round-trip latency");
    std::cout << "\n";

    client.close();
    server.close();
}

// Benchmark: Multiple concurrent socket pairs
void bench_concurrent_latency(int num_pairs, std::size_t message_size, int iterations)
{
    std::cout << "  Concurrent pairs: " << num_pairs << ", ";
    std::cout << "Message size: " << message_size << " bytes, ";
    std::cout << "Iterations: " << iterations << "\n";

    asio::io_context ioc;

    // Store sockets and stats separately for safe reference passing
    std::vector<tcp::socket> clients;
    std::vector<tcp::socket> servers;
    std::vector<bench::statistics> stats(num_pairs);

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    // Launch concurrent ping-pong tasks
    for (int p = 0; p < num_pairs; ++p)
    {
        asio::co_spawn(ioc,
            pingpong_task(clients[p], servers[p], message_size, iterations, stats[p]),
            asio::detached);
    }

    ioc.run();

    std::cout << "  Per-pair results:\n";
    for (int i = 0; i < num_pairs && i < 3; ++i)
    {
        std::cout << "    Pair " << i << ": mean="
                  << bench::format_latency(stats[i].mean())
                  << ", p99=" << bench::format_latency(stats[i].p99())
                  << "\n";
    }
    if (num_pairs > 3)
        std::cout << "    ... (" << (num_pairs - 3) << " more pairs)\n";

    // Calculate average across all pairs
    double total_mean = 0;
    double total_p99 = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }
    std::cout << "  Average mean latency: "
              << bench::format_latency(total_mean / num_pairs) << "\n";
    std::cout << "  Average p99 latency:  "
              << bench::format_latency(total_p99 / num_pairs) << "\n\n";

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();
}

int main()
{
    std::cout << "Boost.Asio Socket Latency Benchmarks\n";
    std::cout << "====================================\n";

    bench::print_header("Ping-Pong Round-Trip Latency (Asio)");

    // Variable message sizes
    std::vector<std::size_t> message_sizes = {1, 64, 1024};
    int iterations = 1000;

    for (auto size : message_sizes)
        bench_pingpong_latency(size, iterations);

    bench::print_header("Concurrent Socket Pairs Latency (Asio)");

    // Multiple concurrent connections
    bench_concurrent_latency(1, 64, 1000);
    bench_concurrent_latency(4, 64, 500);
    bench_concurrent_latency(16, 64, 250);

    std::cout << "\nBenchmarks complete.\n";
    return 0;
}
