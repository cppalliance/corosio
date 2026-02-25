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
#include <thread>
#include <vector>

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
    bench::state& state)
{
    std::vector<char> send_buf(message_size, 'P');
    std::vector<char> recv_buf(message_size);

    while (state.running())
    {
        auto lp = state.lap();

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
    }

    client.shutdown(corosio::tcp_socket::shutdown_send);
}

template<auto Backend>
void
bench_pingpong_latency(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    auto message_size = static_cast<std::size_t>(state.range(0));
    state.counters["message_size"] = static_cast<double>(message_size);

    corosio::native_io_context<Backend> ioc;
    auto [client, server] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    client.set_option(corosio::native_socket_option::no_delay(true));
    server.set_option(corosio::native_socket_option::no_delay(true));

    capy::run_async(ioc.get_executor())(
        pingpong_client_task<Backend>(client, server, message_size, state));

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

template<auto Backend>
void
bench_concurrent_latency(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    int num_pairs = static_cast<int>(state.range(0));
    state.counters["num_pairs"] = num_pairs;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;

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

    for (int p = 0; p < num_pairs; ++p)
    {
        capy::run_async(ioc.get_executor())(
            pingpong_client_task<Backend>(clients[p], servers[p], 64, state));
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

template<auto Backend>
bench::benchmark_suite
make_socket_latency_suite()
{
    using F           = bench::bench_flags;
    using socket_type = corosio::native_tcp_socket<Backend>;

    return bench::benchmark_suite("socket_latency", F::needs_conntrack_drain)
        .set_warmup([]{
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
        })
        .add("pingpong", bench_pingpong_latency<Backend>)
            .args({1, 64, 1024})
        .add("concurrent", bench_concurrent_latency<Backend>)
            .args({1, 4, 16});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_socket_latency_suite)
