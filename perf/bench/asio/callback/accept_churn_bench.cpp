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
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_acceptor;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

// Connect+accept+exchange 1 byte+close, repeat
struct sequential_churn_op
{
    asio::io_context& ioc;
    tcp_acceptor& acc;
    tcp::endpoint ep;
    std::atomic<bool>& running;
    int64_t& cycles;
    perf::statistics& latency_stats;
    std::unique_ptr<tcp_socket> client;
    std::unique_ptr<tcp_socket> server;
    perf::stopwatch sw;
    char byte         = 'X';
    char recv_byte    = 0;
    bool connect_done = false;
    bool accept_done  = false;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;

        sw.reset();
        connect_done = false;
        accept_done  = false;
        client       = std::make_unique<tcp_socket>(ioc.get_executor());
        server       = std::make_unique<tcp_socket>(ioc.get_executor());
        client->open(tcp::v4());
        client->set_option(asio::socket_base::linger(true, 0));

        client->async_connect(ep, [this](boost::system::error_code ec) {
            if (ec)
                return;
            connect_done = true;
            if (accept_done)
                do_write();
        });

        acc.async_accept(*server, [this](boost::system::error_code ec) {
            if (ec)
                return;
            accept_done = true;
            if (connect_done)
                do_write();
        });
    }

    void do_write()
    {
        byte = 'X';
        asio::async_write(
            *client, asio::buffer(&byte, 1),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                do_read();
            });
    }

    void do_read()
    {
        recv_byte = 0;
        asio::async_read(
            *server, asio::buffer(&recv_byte, 1),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                finish();
            });
    }

    void finish()
    {
        client->close();
        server->close();

        latency_stats.add(sw.elapsed_us());
        ++cycles;
        start();
    }
};

// Single connect/accept/1-byte-exchange/close loop. Compared against the
// coroutine variant, the difference isolates coroutine suspend/resume overhead.
bench::benchmark_result
bench_sequential_churn(double duration_s)
{
    perf::print_header("Sequential Accept Churn (Asio Callbacks)");

    asio::io_context ioc;
    tcp_acceptor acc(ioc.get_executor(), tcp::endpoint(tcp::v4(), 0));
    acc.set_option(tcp_acceptor::reuse_address(true));
    auto ep = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};
    int64_t cycles = 0;
    perf::statistics latency_stats;

    sequential_churn_op op{ioc,           acc, ep, running, cycles,
                           latency_stats, {},  {}, {}};

    perf::stopwatch total_sw;

    op.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    timer.join();

    double elapsed       = total_sw.elapsed_seconds();
    double conns_per_sec = static_cast<double>(cycles) / elapsed;

    std::cout << "  Cycles:      " << cycles << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << perf::format_rate(conns_per_sec) << "\n";
    perf::print_latency_stats(latency_stats, "Cycle latency");
    std::cout << "\n";

    acc.close();

    return bench::benchmark_result("sequential")
        .add("cycles", static_cast<double>(cycles))
        .add("elapsed_s", elapsed)
        .add("conns_per_sec", conns_per_sec)
        .add_latency_stats("cycle_latency", latency_stats);
}

// N independent accept loops on separate listeners. Reveals whether
// fd allocation or acceptor state scales linearly under callbacks.
bench::benchmark_result
bench_concurrent_churn(int num_loops, double duration_s)
{
    std::cout << "  Concurrent loops: " << num_loops << "\n";

    asio::io_context ioc;
    std::atomic<bool> running{true};
    std::vector<int64_t> cycle_counts(num_loops, 0);
    std::vector<perf::statistics> stats(num_loops);

    std::vector<std::unique_ptr<tcp_acceptor>> acceptors;
    acceptors.reserve(num_loops);
    for (int i = 0; i < num_loops; ++i)
    {
        acceptors.push_back(
            std::make_unique<tcp_acceptor>(
                ioc.get_executor(), tcp::endpoint(tcp::v4(), 0)));
        acceptors.back()->set_option(tcp_acceptor::reuse_address(true));
    }

    std::vector<std::unique_ptr<sequential_churn_op>> ops;
    ops.reserve(num_loops);

    perf::stopwatch total_sw;

    for (int i = 0; i < num_loops; ++i)
    {
        auto ep = tcp::endpoint(
            asio::ip::address_v4::loopback(),
            acceptors[i]->local_endpoint().port());
        ops.push_back(
            std::make_unique<sequential_churn_op>(sequential_churn_op{
                ioc,
                *acceptors[i],
                ep,
                running,
                cycle_counts[i],
                stats[i],
                {},
                {},
                {}}));
        ops.back()->start();
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_cycles = 0;
    for (auto c : cycle_counts)
        total_cycles += c;

    double conns_per_sec = static_cast<double>(total_cycles) / elapsed;

    double total_mean = 0;
    double total_p99  = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Total cycles: " << total_cycles << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate(conns_per_sec) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency(total_mean / num_loops) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency(total_p99 / num_loops) << "\n\n";

    for (auto& a : acceptors)
        a->close();

    return bench::benchmark_result("concurrent_" + std::to_string(num_loops))
        .add("num_loops", num_loops)
        .add("total_cycles", static_cast<double>(total_cycles))
        .add("conns_per_sec", conns_per_sec)
        .add("avg_mean_latency_us", total_mean / num_loops)
        .add("avg_p99_latency_us", total_p99 / num_loops);
}

// Burst: open N connections, accept all, close all, repeat
struct burst_churn_op
{
    asio::io_context& ioc;
    tcp_acceptor& acc;
    tcp::endpoint ep;
    std::atomic<bool>& running;
    int64_t& total_accepted;
    perf::statistics& burst_stats;
    int burst_size;

    std::vector<std::unique_ptr<tcp_socket>> clients;
    std::vector<std::unique_ptr<tcp_socket>> servers;
    int accepted_count = 0;
    perf::stopwatch sw;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;

        sw.reset();
        clients.clear();
        servers.clear();
        accepted_count = 0;

        clients.reserve(burst_size);
        servers.reserve(burst_size);

        // Initiate all connects and accepts
        for (int i = 0; i < burst_size; ++i)
        {
            clients.push_back(std::make_unique<tcp_socket>(ioc.get_executor()));
            clients.back()->open(tcp::v4());
            clients.back()->set_option(asio::socket_base::linger(true, 0));
            clients.back()->async_connect(ep, [](boost::system::error_code) {});

            servers.push_back(std::make_unique<tcp_socket>(ioc.get_executor()));
            acc.async_accept(
                *servers.back(), [this](boost::system::error_code ec) {
                    if (ec)
                        return;
                    ++accepted_count;
                    ++total_accepted;
                    if (accepted_count == burst_size)
                        close_all();
                });
        }
    }

    void close_all()
    {
        for (auto& c : clients)
            c->close();
        for (auto& s : servers)
            s->close();

        burst_stats.add(sw.elapsed_us());
        start();
    }
};

// Burst N connects then accept all — stresses the listen backlog and
// batched fd creation. Reveals whether the acceptor handles connection
// storms gracefully or suffers from backlog overflow.
bench::benchmark_result
bench_burst_churn(int burst_size, double duration_s)
{
    std::cout << "  Burst size: " << burst_size << "\n";

    asio::io_context ioc;
    tcp_acceptor acc(ioc.get_executor(), tcp::endpoint(tcp::v4(), 0));
    acc.set_option(tcp_acceptor::reuse_address(true));
    auto ep = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};
    int64_t total_accepted = 0;
    perf::statistics burst_stats;

    burst_churn_op op{ioc,         acc,        ep, running, total_accepted,
                      burst_stats, burst_size, {}, {},      {},
                      {}};

    perf::stopwatch total_sw;

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    double elapsed         = total_sw.elapsed_seconds();
    double accepts_per_sec = static_cast<double>(total_accepted) / elapsed;

    std::cout << "    Total accepted: " << total_accepted << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Accept rate: " << perf::format_rate(accepts_per_sec)
              << "\n";
    perf::print_latency_stats(burst_stats, "Burst latency");
    std::cout << "\n";

    acc.close();

    return bench::benchmark_result("burst_" + std::to_string(burst_size))
        .add("burst_size", burst_size)
        .add("total_accepted", static_cast<double>(total_accepted))
        .add("accepts_per_sec", accepts_per_sec)
        .add_latency_stats("burst_latency", burst_stats);
}

} // anonymous namespace

void
run_accept_churn_benchmarks(
    bench::result_collector& collector, char const* filter, double duration_s)
{
    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    if (run_all || std::strcmp(filter, "sequential") == 0)
        collector.add(bench_sequential_churn(duration_s));

    if (run_all || std::strcmp(filter, "concurrent") == 0)
    {
        perf::print_header("Concurrent Accept Churn (Asio Callbacks)");
        collector.add(bench_concurrent_churn(1, duration_s));
        collector.add(bench_concurrent_churn(4, duration_s));
        collector.add(bench_concurrent_churn(16, duration_s));
    }

    if (run_all || std::strcmp(filter, "burst") == 0)
    {
        perf::print_header("Burst Accept Churn (Asio Callbacks)");
        collector.add(bench_burst_churn(10, duration_s));
        collector.add(bench_burst_churn(100, duration_s));
    }
}

} // namespace asio_callback_bench
