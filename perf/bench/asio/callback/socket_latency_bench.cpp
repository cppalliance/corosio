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
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

struct pingpong_op
{
    enum phase
    {
        write_client,
        read_server,
        write_server,
        read_client
    };

    tcp_socket& client;
    tcp_socket& server;
    std::vector<char> send_buf;
    std::vector<char> recv_buf;
    bench::state& state;
    perf::stopwatch sw;
    phase phase_;

    pingpong_op(
        tcp_socket& c,
        tcp_socket& s,
        std::size_t message_size,
        bench::state& st)
        : client(c)
        , server(s)
        , send_buf(message_size, 'P')
        , recv_buf(message_size)
        , state(st)
        , phase_(write_client)
    {
    }

    void start()
    {
        if (!state.running())
        {
            client.shutdown(tcp_socket::shutdown_send);
            return;
        }
        sw.reset();
        phase_ = write_client;
        do_step();
    }

    void do_step()
    {
        switch (phase_)
        {
        case write_client:
            asio::async_write(
                client, asio::buffer(send_buf),
                [this](boost::system::error_code ec, std::size_t) {
                    if (ec)
                        return;
                    phase_ = read_server;
                    do_step();
                });
            break;

        case read_server:
            asio::async_read(
                server, asio::buffer(recv_buf),
                [this](boost::system::error_code ec, std::size_t) {
                    if (ec)
                        return;
                    phase_ = write_server;
                    do_step();
                });
            break;

        case write_server:
            asio::async_write(
                server, asio::buffer(recv_buf),
                [this](boost::system::error_code ec, std::size_t) {
                    if (ec)
                        return;
                    phase_ = read_client;
                    do_step();
                });
            break;

        case read_client:
            asio::async_read(
                client, asio::buffer(recv_buf),
                [this](boost::system::error_code ec, std::size_t) {
                    if (ec)
                        return;
                    state.latency().add(sw.elapsed_ns());
                    state.ops().fetch_add(1, std::memory_order_relaxed);
                    start();
                });
            break;
        }
    }
};

void
bench_pingpong_latency(bench::state& state)
{
    auto message_size = static_cast<std::size_t>(state.range(0));
    state.counters["message_size"] = static_cast<double>(message_size);

    asio::io_context ioc;
    auto [client, server] = asio_bench::make_socket_pair(ioc);

    pingpong_op op(client, server, message_size, state);

    op.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    client.close();
    server.close();
}

void
bench_concurrent_latency(bench::state& state)
{
    int num_pairs = static_cast<int>(state.range(0));
    state.counters["num_pairs"] = num_pairs;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    // Stable addresses needed for concurrent ops
    std::vector<std::unique_ptr<pingpong_op>> ops;
    ops.reserve(num_pairs);
    for (int p = 0; p < num_pairs; ++p)
    {
        ops.push_back(
            std::make_unique<pingpong_op>(
                clients[p], servers[p], 64, state));
        ops.back()->start();
    }

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();
}

} // anonymous namespace

bench::benchmark_suite
make_socket_latency_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("socket_latency", F::needs_conntrack_drain)
        .set_warmup([] {
            asio::io_context ioc;
            auto [c, s]  = asio_bench::make_socket_pair(ioc);
            char buf[64] = {};
            for (int i = 0; i < 100; ++i)
            {
                asio::write(c, asio::buffer(buf));
                asio::read(s, asio::buffer(buf));
            }
            c.close();
            s.close();
        })
        .add("pingpong", bench_pingpong_latency)
            .args({1, 64, 1024})
        .add("concurrent", bench_concurrent_latency)
            .args({1, 4, 16});
}

} // namespace asio_callback_bench
