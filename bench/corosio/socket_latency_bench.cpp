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

#include <iostream>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Ping-pong coroutine task
capy::task<> pingpong_task(
    corosio::socket& client,
    corosio::socket& server,
    std::size_t message_size,
    int iterations,
    bench::statistics& stats)
{
    std::vector<char> send_buf(message_size, 'P');
    std::vector<char> recv_buf(message_size);

    for (int i = 0; i < iterations; ++i)
    {
        bench::stopwatch sw;

        // Client sends ping
        auto [ec1, n1] = co_await client.write_some(
            capy::const_buffer(send_buf.data(), send_buf.size()));
        if (ec1)
        {
            std::cerr << "    Write error: " << ec1.message() << "\n";
            co_return;
        }

        // Server receives ping
        auto [ec2, n2] = co_await server.read_some(
            capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec2)
        {
            std::cerr << "    Server read error: " << ec2.message() << "\n";
            co_return;
        }

        // Server sends pong
        auto [ec3, n3] = co_await server.write_some(
            capy::const_buffer(recv_buf.data(), n2));
        if (ec3)
        {
            std::cerr << "    Server write error: " << ec3.message() << "\n";
            co_return;
        }

        // Client receives pong
        auto [ec4, n4] = co_await client.read_some(
            capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec4)
        {
            std::cerr << "    Client read error: " << ec4.message() << "\n";
            co_return;
        }

        double rtt_us = sw.elapsed_us();
        stats.add(rtt_us);
    }
}

// Benchmark: Ping-pong latency measurement
void bench_pingpong_latency(std::size_t message_size, int iterations)
{
    std::cout << "  Message size: " << message_size << " bytes, ";
    std::cout << "Iterations: " << iterations << "\n";

    corosio::io_context ioc;
    auto [client, server] = corosio::test::make_socket_pair(ioc);

    // Disable Nagle's algorithm for low latency
    client.set_no_delay(true);
    server.set_no_delay(true);

    bench::statistics latency_stats;

    capy::run_async(ioc.get_executor())(
        pingpong_task(client, server, message_size, iterations, latency_stats));
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

    corosio::io_context ioc;

    // Store sockets and stats separately for safe reference passing
    std::vector<corosio::socket> clients;
    std::vector<corosio::socket> servers;
    std::vector<bench::statistics> stats(num_pairs);

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair(ioc);
        // Disable Nagle's algorithm for low latency
        c.set_no_delay(true);
        s.set_no_delay(true);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    // Launch concurrent ping-pong tasks
    for (int p = 0; p < num_pairs; ++p)
    {
        capy::run_async(ioc.get_executor())(
            pingpong_task(clients[p], servers[p], message_size, iterations, stats[p]));
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
    std::cout << "Boost.Corosio Socket Latency Benchmarks\n";
    std::cout << "=======================================\n";

    bench::print_header("Ping-Pong Round-Trip Latency");

    // Variable message sizes
    std::vector<std::size_t> message_sizes = {1, 64, 1024};
    int iterations = 1000;

    for (auto size : message_sizes)
        bench_pingpong_latency(size, iterations);

    bench::print_header("Concurrent Socket Pairs Latency");

    // Multiple concurrent connections
    bench_concurrent_latency(1, 64, 1000);
    bench_concurrent_latency(4, 64, 500);
    bench_concurrent_latency(16, 64, 250);

    std::cout << "\nBenchmarks complete.\n";
    return 0;
}
