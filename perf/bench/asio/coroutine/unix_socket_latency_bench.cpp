//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include "../unix_socket_utils.hpp"

#include <boost/asio/detail/concurrency_hint.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio_bench {
namespace {

asio::awaitable<void, executor_type>
pingpong_client_task(
    local_socket& client,
    local_socket& server,
    std::size_t message_size,
    std::atomic<bool>& running,
    bench::state& state)
{
    std::vector<char> send_buf(message_size, 'P');
    std::vector<char> recv_buf(message_size);

    try
    {
        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            co_await asio::async_write(
                client, asio::buffer(send_buf.data(), send_buf.size()),
                asio::deferred);

            co_await asio::async_read(
                server, asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::deferred);

            co_await asio::async_write(
                server, asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::deferred);

            co_await asio::async_read(
                client, asio::buffer(recv_buf.data(), recv_buf.size()),
                asio::deferred);
        }

        client.shutdown(local_socket::shutdown_send);
    }
    catch (std::exception const&)
    {
    }
}

void
bench_pingpong_latency_impl(bench::state& state, bool lockless)
{
    auto message_size = static_cast<std::size_t>(state.range(0));
    state.counters["message_size"] = static_cast<double>(message_size);

    asio::io_context ioc(lockless ? BOOST_ASIO_CONCURRENCY_HINT_UNSAFE : 1);
    auto [client, server] = make_unix_socket_pair(ioc);

    std::atomic<bool> running{true};

    asio::co_spawn(
        ioc,
        pingpong_client_task(
            client, server, message_size, running, state),
        asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    perf::stopwatch sw;
    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    client.close();
    server.close();
}

void bench_pingpong_latency(bench::state& s) { bench_pingpong_latency_impl(s, false); }
void bench_pingpong_latency_lockless(bench::state& s) { bench_pingpong_latency_impl(s, true); }

void
bench_concurrent_latency_impl(bench::state& state, bool lockless)
{
    int num_pairs = static_cast<int>(state.range(0));
    state.counters["num_pairs"] = num_pairs;

    asio::io_context ioc(lockless ? BOOST_ASIO_CONCURRENCY_HINT_UNSAFE : 1);

    std::vector<local_socket> clients;
    std::vector<local_socket> servers;

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        auto [c, s] = make_unix_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    std::atomic<bool> running{true};

    for (int p = 0; p < num_pairs; ++p)
    {
        asio::co_spawn(
            ioc,
            pingpong_client_task(
                clients[p], servers[p], 64, running, state),
            asio::detached);
    }

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
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

void bench_concurrent_latency(bench::state& s) { bench_concurrent_latency_impl(s, false); }
void bench_concurrent_latency_lockless(bench::state& s) { bench_concurrent_latency_impl(s, true); }

} // anonymous namespace

bench::benchmark_suite
make_unix_socket_latency_suite()
{
    return bench::benchmark_suite("unix_socket_latency")
        .set_warmup([] {
            asio::io_context ioc;
            auto [c, s]  = make_unix_socket_pair(ioc);
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
        .add("pingpong_lockless", bench_pingpong_latency_lockless)
            .args({1, 64, 1024})
        .add("concurrent", bench_concurrent_latency)
            .args({1, 4, 16})
        .add("concurrent_lockless", bench_concurrent_latency_lockless)
            .args({1, 4, 16});
}

} // namespace asio_bench
