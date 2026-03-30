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
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

// Minimal kernel buffers and immediate RST on close to avoid TIME_WAIT
static void
configure_churn_socket(corosio::tcp_socket& s)
{
    s.set_option(corosio::native_socket_option::send_buffer_size(1024));
    s.set_option(corosio::native_socket_option::receive_buffer_size(1024));
    s.set_option(corosio::native_socket_option::linger(true, 0));
}

// Single connect/accept/1-byte-exchange/close loop
template<auto Backend>
void
bench_sequential_churn(bench::state& state)
{
    using socket_type   = corosio::native_tcp_socket<Backend>;
    using acceptor_type = corosio::native_tcp_acceptor<Backend>;

    corosio::native_io_context<Backend> ioc;
    acceptor_type acc(ioc);
    acc.open();
    acc.set_option(corosio::native_socket_option::reuse_address(true));

    if (auto ec =
            acc.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "  Bind failed: " << ec.message() << "\n";
        return;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "  Listen failed: " << ec.message() << "\n";
        return;
    }

    auto ep = acc.local_endpoint();

    auto task = [&]() -> capy::task<> {
        while (state.running())
        {
            auto lp = state.lap();

            socket_type client(ioc);
            socket_type server(ioc);
            client.open();
            configure_churn_socket(client);

            capy::run_async(ioc.get_executor())(
                [](socket_type& c, corosio::endpoint ep) -> capy::task<> {
                    auto [ec] = co_await c.connect(ep);
                    (void)ec;
                }(client, ep));

            auto [aec] = co_await acc.accept(server);
            if (aec)
                co_return;

            char byte = 'X';
            auto [wec, wn] =
                co_await capy::write(client, capy::const_buffer(&byte, 1));
            if (wec)
                co_return;

            char recv = 0;
            auto [rec, rn] =
                co_await capy::read(server, capy::mutable_buffer(&recv, 1));
            if (rec)
                co_return;

            client.close();
            server.close();
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
        ioc.stop();
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

template<auto Backend>
void
bench_sequential_churn_lockless(bench::state& state)
{
    using socket_type   = corosio::native_tcp_socket<Backend>;
    using acceptor_type = corosio::native_tcp_acceptor<Backend>;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts);
    acceptor_type acc(ioc);
    acc.open();
    acc.set_option(corosio::native_socket_option::reuse_address(true));

    if (auto ec =
            acc.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "  Bind failed: " << ec.message() << "\n";
        return;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "  Listen failed: " << ec.message() << "\n";
        return;
    }

    auto ep = acc.local_endpoint();

    auto task = [&]() -> capy::task<> {
        while (state.running())
        {
            auto lp = state.lap();

            socket_type client(ioc);
            socket_type server(ioc);
            client.open();
            configure_churn_socket(client);

            capy::run_async(ioc.get_executor())(
                [](socket_type& c, corosio::endpoint ep) -> capy::task<> {
                    auto [ec] = co_await c.connect(ep);
                    (void)ec;
                }(client, ep));

            auto [aec] = co_await acc.accept(server);
            if (aec)
                co_return;

            char byte = 'X';
            auto [wec, wn] =
                co_await capy::write(client, capy::const_buffer(&byte, 1));
            if (wec)
                co_return;

            char recv = 0;
            auto [rec, rn] =
                co_await capy::read(server, capy::mutable_buffer(&recv, 1));
            if (rec)
                co_return;

            client.close();
            server.close();
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
        ioc.stop();
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

// N independent accept loops on separate listeners
template<auto Backend>
void
bench_concurrent_churn(bench::state& state)
{
    using socket_type   = corosio::native_tcp_socket<Backend>;
    using acceptor_type = corosio::native_tcp_acceptor<Backend>;

    int num_loops = static_cast<int>(state.range(0));
    state.counters["num_loops"] = num_loops;

    corosio::native_io_context<Backend> ioc;

    std::vector<acceptor_type> acceptors;
    acceptors.reserve(num_loops);
    for (int i = 0; i < num_loops; ++i)
    {
        acceptors.emplace_back(ioc);
        auto& acc = acceptors.back();
        acc.open();
        acc.set_option(corosio::native_socket_option::reuse_address(true));
        if (auto ec = acc.bind(
                corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
        {
            std::cerr << "  Bind failed: " << ec.message() << "\n";
            return;
        }
        if (auto ec = acc.listen())
        {
            std::cerr << "  Listen failed: " << ec.message() << "\n";
            return;
        }
    }

    auto loop_task = [&](int idx) -> capy::task<> {
        auto& acc = acceptors[idx];
        auto ep   = acc.local_endpoint();

        while (state.running())
        {
            auto lp = state.lap();

            socket_type client(ioc);
            socket_type server(ioc);
            client.open();
            configure_churn_socket(client);

            capy::run_async(ioc.get_executor())(
                [](socket_type& c, corosio::endpoint ep) -> capy::task<> {
                    auto [ec] = co_await c.connect(ep);
                    (void)ec;
                }(client, ep));

            auto [aec] = co_await acc.accept(server);
            if (aec)
                co_return;

            char byte = 'X';
            auto [wec, wn] =
                co_await capy::write(client, capy::const_buffer(&byte, 1));
            if (wec)
                co_return;

            char recv = 0;
            auto [rec, rn] =
                co_await capy::read(server, capy::mutable_buffer(&recv, 1));
            if (rec)
                co_return;

            client.close();
            server.close();
        }
    };

    perf::stopwatch sw;

    for (int i = 0; i < num_loops; ++i)
        capy::run_async(ioc.get_executor())(loop_task(i));

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    for (auto& a : acceptors)
        a.close();
}

// Burst N connects then accept all
template<auto Backend>
void
bench_burst_churn(bench::state& state)
{
    using socket_type   = corosio::native_tcp_socket<Backend>;
    using acceptor_type = corosio::native_tcp_acceptor<Backend>;

    int burst_size = static_cast<int>(state.range(0));
    state.counters["burst_size"] = burst_size;

    corosio::native_io_context<Backend> ioc;
    acceptor_type acc(ioc);
    acc.open();
    acc.set_option(corosio::native_socket_option::reuse_address(true));

    if (auto ec =
            acc.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "  Bind failed: " << ec.message() << "\n";
        return;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "  Listen failed: " << ec.message() << "\n";
        return;
    }

    auto ep = acc.local_endpoint();

    auto task = [&]() -> capy::task<> {
        while (state.running())
        {
            auto lp = state.lap();

            std::vector<socket_type> clients;
            std::vector<socket_type> servers;
            clients.reserve(burst_size);
            servers.reserve(burst_size);

            for (int i = 0; i < burst_size; ++i)
            {
                clients.emplace_back(ioc);
                clients.back().open();
                configure_churn_socket(clients.back());
                capy::run_async(ioc.get_executor())(
                    [](socket_type& c, corosio::endpoint ep) -> capy::task<> {
                        auto [ec] = co_await c.connect(ep);
                        (void)ec;
                    }(clients.back(), ep));
            }

            for (int i = 0; i < burst_size; ++i)
            {
                servers.emplace_back(ioc);
                auto [aec] = co_await acc.accept(servers.back());
                if (aec)
                    co_return;
            }

            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(task());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

template<auto Backend>
void
bench_burst_churn_lockless(bench::state& state)
{
    using socket_type   = corosio::native_tcp_socket<Backend>;
    using acceptor_type = corosio::native_tcp_acceptor<Backend>;

    int burst_size = static_cast<int>(state.range(0));
    state.counters["burst_size"] = burst_size;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts);
    acceptor_type acc(ioc);
    acc.open();
    acc.set_option(corosio::native_socket_option::reuse_address(true));

    if (auto ec =
            acc.bind(corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
    {
        std::cerr << "  Bind failed: " << ec.message() << "\n";
        return;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "  Listen failed: " << ec.message() << "\n";
        return;
    }

    auto ep = acc.local_endpoint();

    auto task = [&]() -> capy::task<> {
        while (state.running())
        {
            auto lp = state.lap();

            std::vector<socket_type> clients;
            std::vector<socket_type> servers;
            clients.reserve(burst_size);
            servers.reserve(burst_size);

            for (int i = 0; i < burst_size; ++i)
            {
                clients.emplace_back(ioc);
                clients.back().open();
                configure_churn_socket(clients.back());
                capy::run_async(ioc.get_executor())(
                    [](socket_type& c, corosio::endpoint ep) -> capy::task<> {
                        auto [ec] = co_await c.connect(ep);
                        (void)ec;
                    }(clients.back(), ep));
            }

            for (int i = 0; i < burst_size; ++i)
            {
                servers.emplace_back(ioc);
                auto [aec] = co_await acc.accept(servers.back());
                if (aec)
                    co_return;
            }

            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(task());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_accept_churn_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("accept_churn", F::needs_conntrack_drain)
        .add("sequential", bench_sequential_churn<Backend>)
        .add("sequential_lockless", bench_sequential_churn_lockless<Backend>)
        .add("concurrent", bench_concurrent_churn<Backend>)
            .args({1, 4, 16})
        .add("burst", bench_burst_churn<Backend>)
            .args({10, 100})
        .add("burst_lockless", bench_burst_churn_lockless<Backend>)
            .args({10, 100});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_accept_churn_suite)
