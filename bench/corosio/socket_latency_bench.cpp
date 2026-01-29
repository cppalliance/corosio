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
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <cstring>
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
        auto [ec1, n1] = co_await capy::write(
            client, capy::const_buffer(send_buf.data(), send_buf.size()));
        if (ec1)
        {
            std::cerr << "    Write error: " << ec1.message() << "\n";
            co_return;
        }

        // Server receives ping
        auto [ec2, n2] = co_await capy::read(
            server, capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec2)
        {
            std::cerr << "    Server read error: " << ec2.message() << "\n";
            co_return;
        }

        // Server sends pong
        auto [ec3, n3] = co_await capy::write(
            server, capy::const_buffer(recv_buf.data(), n2));
        if (ec3)
        {
            std::cerr << "    Server write error: " << ec3.message() << "\n";
            co_return;
        }

        // Client receives pong
        auto [ec4, n4] = co_await capy::read(
            client, capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec4)
        {
            std::cerr << "    Client read error: " << ec4.message() << "\n";
            co_return;
        }

        double rtt_us = sw.elapsed_us();
        stats.add(rtt_us);
    }
}

// Measures round-trip latency for a request-response pattern over loopback sockets.
// Client sends a message, server echoes it back, measuring the complete cycle time.
// This is the fundamental latency metric for RPC-style protocols. Reports mean,
// median (p50), and tail latencies (p99, p99.9) which are critical for SLA compliance.
// Different message sizes reveal fixed overhead vs. size-dependent costs.
bench::benchmark_result bench_pingpong_latency(std::size_t message_size, int iterations)
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

    return bench::benchmark_result("pingpong_" + std::to_string(message_size))
        .add("message_size", static_cast<double>(message_size))
        .add("iterations", iterations)
        .add_latency_stats("rtt", latency_stats);
}

// Measures latency degradation under concurrent connection load. Multiple socket
// pairs perform ping-pong simultaneously, revealing how latency increases as the
// scheduler multiplexes more connections. Critical for capacity planning: shows
// how many concurrent connections can be sustained before latency becomes
// unacceptable. A well-designed scheduler should show gradual degradation rather
// than sudden latency spikes.
bench::benchmark_result bench_concurrent_latency(int num_pairs, std::size_t message_size, int iterations)
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

    return bench::benchmark_result("concurrent_" + std::to_string(num_pairs) + "_pairs")
        .add("num_pairs", num_pairs)
        .add("message_size", static_cast<double>(message_size))
        .add("iterations", iterations)
        .add("avg_mean_latency_us", total_mean / num_pairs)
        .add("avg_p99_latency_us", total_p99 / num_pairs);
}

// Run benchmarks
void run_benchmarks(const char* output_file, const char* bench_filter)
{
    std::cout << "Boost.Corosio Socket Latency Benchmarks\n";
    std::cout << "=======================================\n";

    bench::result_collector collector("corosio");

    bool run_all = !bench_filter || std::strcmp(bench_filter, "all") == 0;

    // Variable message sizes
    std::vector<std::size_t> message_sizes = {1, 64, 1024};
    int iterations = 1000;

    if (run_all || std::strcmp(bench_filter, "pingpong") == 0)
    {
        bench::print_header("Ping-Pong Round-Trip Latency");
        for (auto size : message_sizes)
            collector.add(bench_pingpong_latency(size, iterations));
    }

    if (run_all || std::strcmp(bench_filter, "concurrent") == 0)
    {
        bench::print_header("Concurrent Socket Pairs Latency");
        collector.add(bench_concurrent_latency(1, 64, 1000));
        collector.add(bench_concurrent_latency(4, 64, 500));
        collector.add(bench_concurrent_latency(16, 64, 250));
    }

    std::cout << "\nBenchmarks complete.\n";

    if (output_file)
    {
        if (collector.write_json(output_file))
            std::cout << "Results written to: " << output_file << "\n";
        else
            std::cerr << "Error: Failed to write results to: " << output_file << "\n";
    }
}

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --bench <name>     Run only the specified benchmark\n";
    std::cout << "  --output <file>    Write JSON results to file\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    std::cout << "Available benchmarks:\n";
    std::cout << "  pingpong           Ping-pong round-trip latency (various message sizes)\n";
    std::cout << "  concurrent         Concurrent socket pairs latency\n";
    std::cout << "  all                Run all benchmarks (default)\n";
}

int main(int argc, char* argv[])
{
    const char* output_file = nullptr;
    const char* bench_filter = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--bench") == 0)
        {
            if (i + 1 < argc)
            {
                bench_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --bench requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--output") == 0)
        {
            if (i + 1 < argc)
            {
                output_file = argv[++i];
            }
            else
            {
                std::cerr << "Error: --output requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    run_benchmarks(output_file, bench_filter);
    return 0;
}
