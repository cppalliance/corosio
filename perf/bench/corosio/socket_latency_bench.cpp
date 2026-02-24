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
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"
#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

template<auto Backend>
capy::task<>
pingpong_client_task(
    corosio::native_tcp_socket<Backend>& client,
    corosio::native_tcp_socket<Backend>& server,
    std::size_t message_size,
    std::atomic<bool>& running,
    int64_t& iterations,
    perf::statistics& stats)
{
    std::vector<char> send_buf(message_size, 'P');
    std::vector<char> recv_buf(message_size);

    while (running.load(std::memory_order_relaxed))
    {
        perf::stopwatch sw;

        auto [ec1, n1] = co_await capy::write(
            client, capy::const_buffer(send_buf.data(), send_buf.size()));
        if (ec1)
            co_return;

        auto [ec2, n2] = co_await capy::read(
            server, capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec2)
            co_return;

        auto [ec3, n3] = co_await capy::write(
            server, capy::const_buffer(recv_buf.data(), n2));
        if (ec3)
            co_return;

        auto [ec4, n4] = co_await capy::read(
            client, capy::mutable_buffer(recv_buf.data(), recv_buf.size()));
        if (ec4)
            co_return;

        double rtt_us = sw.elapsed_us();
        stats.add(rtt_us);
        ++iterations;
    }

    client.shutdown(corosio::tcp_socket::shutdown_send);
}

template<auto Backend>
bench::benchmark_result
bench_pingpong_latency(std::size_t message_size, double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    std::cout << "  Message size: " << message_size << " bytes\n";

    corosio::native_io_context<Backend> ioc;
    auto [client, server] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    client.set_option(corosio::native_socket_option::no_delay(true));
    server.set_option(corosio::native_socket_option::no_delay(true));

    std::atomic<bool> running{true};
    int64_t iterations = 0;
    perf::statistics latency_stats;

    capy::run_async(ioc.get_executor())(pingpong_client_task<Backend>(
        client, server, message_size, running, iterations, latency_stats));

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    perf::print_latency_stats(latency_stats, "Round-trip latency");
    std::cout << "  Iterations: " << iterations << "\n\n";

    client.close();
    server.close();

    return bench::benchmark_result("pingpong_" + std::to_string(message_size))
        .add("message_size", static_cast<double>(message_size))
        .add("iterations", static_cast<double>(iterations))
        .add_latency_stats("rtt", latency_stats);
}

template<auto Backend>
bench::benchmark_result
bench_concurrent_latency(
    int num_pairs, std::size_t message_size, double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    std::cout << "  Concurrent pairs: " << num_pairs << ", ";
    std::cout << "Message size: " << message_size << " bytes\n";

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    std::vector<perf::statistics> stats(num_pairs);
    std::vector<int64_t> iters(num_pairs, 0);

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    std::atomic<bool> running{true};

    for (int p = 0; p < num_pairs; ++p)
    {
        capy::run_async(ioc.get_executor())(pingpong_client_task<Backend>(
            clients[p], servers[p], message_size, running, iters[p], stats[p]));
    }

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    std::cout << "  Per-pair results:\n";
    for (int i = 0; i < num_pairs && i < 3; ++i)
    {
        std::cout << "    Pair " << i
                  << ": mean=" << perf::format_latency(stats[i].mean())
                  << ", p99=" << perf::format_latency(stats[i].p99())
                  << ", iters=" << iters[i] << "\n";
    }
    if (num_pairs > 3)
        std::cout << "    ... (" << (num_pairs - 3) << " more pairs)\n";

    double total_mean = 0;
    double total_p99  = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }
    std::cout << "  Average mean latency: "
              << perf::format_latency(total_mean / num_pairs) << "\n";
    std::cout << "  Average p99 latency:  "
              << perf::format_latency(total_p99 / num_pairs) << "\n\n";

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();

    return bench::benchmark_result(
               "concurrent_" + std::to_string(num_pairs) + "_pairs")
        .add("num_pairs", num_pairs)
        .add("message_size", static_cast<double>(message_size))
        .add("avg_mean_latency_us", total_mean / num_pairs)
        .add("avg_p99_latency_us", total_p99 / num_pairs);
}

} // anonymous namespace

template<auto Backend>
void
run_socket_latency_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    (void)factory;

    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    // Warm up
    {
        corosio::native_io_context<Backend> ioc;
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        char buf[64] = {};
        auto task    = [&]() -> capy::task<> {
            for (int i = 0; i < 100; ++i)
            {
                (void)co_await c.write_some(
                    capy::const_buffer(buf, sizeof(buf)));
                (void)co_await s.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
            }
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        c.close();
        s.close();
    }

    std::vector<std::size_t> message_sizes = {1, 64, 1024};

    if (run_all || std::strcmp(filter, "pingpong") == 0)
    {
        perf::print_header("Ping-Pong Round-Trip Latency (Corosio)");
        for (auto size : message_sizes)
            collector.add(bench_pingpong_latency<Backend>(size, duration_s));
    }

    if (run_all || std::strcmp(filter, "concurrent") == 0)
    {
        perf::print_header("Concurrent Socket Pairs Latency (Corosio)");
        collector.add(bench_concurrent_latency<Backend>(1, 64, duration_s));
        collector.add(bench_concurrent_latency<Backend>(4, 64, duration_s));
        collector.add(bench_concurrent_latency<Backend>(16, 64, duration_s));
    }
}

} // namespace corosio_bench

COROSIO_BENCH_INSTANTIATE(void corosio_bench::run_socket_latency_benchmarks)
