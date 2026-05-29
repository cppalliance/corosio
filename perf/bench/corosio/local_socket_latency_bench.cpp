//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include <boost/corosio/detail/platform.hpp>
#include "../../common/native_includes.hpp"

#if BOOST_COROSIO_POSIX

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/native/native_local_stream_acceptor.hpp>
#include <boost/corosio/native/native_local_stream_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

template<auto Backend>
capy::task<>
unix_pingpong_client_task(
    corosio::native_local_stream_socket<Backend>& client,
    corosio::native_local_stream_socket<Backend>& server,
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

    client.shutdown(corosio::local_stream_socket::shutdown_send);
}

template<auto Backend>
void
bench_unix_pingpong_latency(bench::state& state)
{
    using socket_type = corosio::native_local_stream_socket<Backend>;

    auto message_size = static_cast<std::size_t>(state.range(0));
    state.counters["message_size"] = static_cast<double>(message_size);

    corosio::native_io_context<Backend> ioc;
    socket_type client(ioc), server(ioc);
    if (auto ec = corosio::connect_pair(client, server))
        throw std::system_error(ec, "connect_pair");

    capy::run_async(ioc.get_executor())(
        unix_pingpong_client_task<Backend>(client, server, message_size, state));

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
bench_unix_concurrent_latency(bench::state& state)
{
    using socket_type = corosio::native_local_stream_socket<Backend>;

    int num_pairs = static_cast<int>(state.range(0));
    state.counters["num_pairs"] = num_pairs;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        socket_type c(ioc), s(ioc);
        if (auto ec = corosio::connect_pair(c, s))
            throw std::system_error(ec, "connect_pair");
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int p = 0; p < num_pairs; ++p)
    {
        capy::run_async(ioc.get_executor())(
            unix_pingpong_client_task<Backend>(clients[p], servers[p], 64, state));
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

template<auto Backend>
void
bench_unix_pingpong_latency_lockless(bench::state& state)
{
    using socket_type = corosio::native_local_stream_socket<Backend>;

    auto message_size = static_cast<std::size_t>(state.range(0));
    state.counters["message_size"] = static_cast<double>(message_size);

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts, 1);
    socket_type client(ioc), server(ioc);
    if (auto ec = corosio::connect_pair(client, server))
        throw std::system_error(ec, "connect_pair");

    capy::run_async(ioc.get_executor())(
        unix_pingpong_client_task<Backend>(client, server, message_size, state));

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
bench_unix_concurrent_latency_lockless(bench::state& state)
{
    using socket_type = corosio::native_local_stream_socket<Backend>;

    int num_pairs = static_cast<int>(state.range(0));
    state.counters["num_pairs"] = num_pairs;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts, 1);

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;

    clients.reserve(num_pairs);
    servers.reserve(num_pairs);

    for (int i = 0; i < num_pairs; ++i)
    {
        socket_type c(ioc), s(ioc);
        if (auto ec = corosio::connect_pair(c, s))
            throw std::system_error(ec, "connect_pair");
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int p = 0; p < num_pairs; ++p)
    {
        capy::run_async(ioc.get_executor())(
            unix_pingpong_client_task<Backend>(clients[p], servers[p], 64, state));
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
make_local_socket_latency_suite()
{
    using F = bench::bench_flags;

    return bench::benchmark_suite("local_socket_latency", F::none)
        .add("pingpong", bench_unix_pingpong_latency<Backend>)
            .args({1, 64, 1024})
        .add("pingpong_lockless", bench_unix_pingpong_latency_lockless<Backend>)
            .args({1, 64, 1024})
        .add("concurrent", bench_unix_concurrent_latency<Backend>)
            .args({1, 4, 16})
        .add("concurrent_lockless", bench_unix_concurrent_latency_lockless<Backend>)
            .args({1, 4, 16});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE_POSIX(corosio_bench::make_local_socket_latency_suite)

#endif // BOOST_COROSIO_POSIX
