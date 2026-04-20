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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

namespace asio_bench {
namespace {

// Minimal kernel buffers and immediate RST on close to avoid TIME_WAIT
static void
configure_churn_socket(tcp_socket& s)
{
    s.set_option(asio::socket_base::send_buffer_size(1024));
    s.set_option(asio::socket_base::receive_buffer_size(1024));
    s.set_option(asio::socket_base::linger(true, 0));
}

// Retry acceptor creation; rapid churn can temporarily exhaust buffers
static tcp_acceptor
make_churn_acceptor(asio::io_context& ioc)
{
    boost::system::error_code ec;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        tcp_acceptor acc(ioc.get_executor());
        ec = acc.open(tcp::v4(), ec);
        if (!ec)
            ec = acc.set_option(tcp_acceptor::reuse_address(true), ec);
        if (!ec)
            ec = acc.bind(tcp::endpoint(tcp::v4(), 0), ec);
        if (!ec)
            ec = acc.listen(asio::socket_base::max_listen_connections, ec);
        if (!ec)
            return acc;
    }
    throw boost::system::system_error(ec);
}

// Single connect/accept/1-byte-exchange/close loop
void
bench_sequential_churn(bench::state& state)
{
    asio::io_context ioc;
    auto acc = make_churn_acceptor(ioc);
    auto ep  = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto lp = state.lap();

                tcp_socket client(ioc.get_executor());
                tcp_socket server(ioc.get_executor());
                boost::system::error_code ec;
                ec = client.open(tcp::v4(), ec);
                if (ec)
                    continue;
                configure_churn_socket(client);

                asio::co_spawn(
                    ioc,
                    [](tcp_socket& c, tcp::endpoint ep)
                        -> asio::awaitable<void, executor_type> {
                        co_await c.async_connect(ep, asio::deferred);
                    }(client, ep),
                    asio::detached);

                server = co_await acc.async_accept(asio::deferred);

                char byte = 'X';
                co_await asio::async_write(
                    client, asio::buffer(&byte, 1), asio::deferred);

                char recv = 0;
                co_await asio::async_read(
                    server, asio::buffer(&recv, 1), asio::deferred);

                client.close();
                server.close();
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

void
bench_sequential_churn_lockless(bench::state& state)
{
    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto acc = make_churn_acceptor(ioc);
    auto ep  = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto lp = state.lap();

                tcp_socket client(ioc.get_executor());
                tcp_socket server(ioc.get_executor());
                boost::system::error_code ec;
                ec = client.open(tcp::v4(), ec);
                if (ec)
                    continue;
                configure_churn_socket(client);

                asio::co_spawn(
                    ioc,
                    [](tcp_socket& c, tcp::endpoint ep)
                        -> asio::awaitable<void, executor_type> {
                        co_await c.async_connect(ep, asio::deferred);
                    }(client, ep),
                    asio::detached);

                server = co_await acc.async_accept(asio::deferred);

                char byte = 'X';
                co_await asio::async_write(
                    client, asio::buffer(&byte, 1), asio::deferred);

                char recv = 0;
                co_await asio::async_read(
                    server, asio::buffer(&recv, 1), asio::deferred);

                client.close();
                server.close();
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

// N independent accept loops on separate listeners
void
bench_concurrent_churn(bench::state& state)
{
    int num_loops = static_cast<int>(state.range(0));
    state.counters["num_loops"] = num_loops;

    asio::io_context ioc;
    std::atomic<bool> running{true};

    std::vector<tcp_acceptor> acceptors;
    acceptors.reserve(num_loops);
    for (int i = 0; i < num_loops; ++i)
        acceptors.push_back(make_churn_acceptor(ioc));

    auto loop_task = [&](int idx) -> asio::awaitable<void, executor_type> {
        auto& acc = acceptors[idx];
        auto ep   = tcp::endpoint(
            asio::ip::address_v4::loopback(), acc.local_endpoint().port());

        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto lp = state.lap();

                tcp_socket client(ioc.get_executor());
                tcp_socket server(ioc.get_executor());
                boost::system::error_code ec;
                ec = client.open(tcp::v4(), ec);
                if (ec)
                    continue;
                configure_churn_socket(client);

                asio::co_spawn(
                    ioc,
                    [](tcp_socket& c, tcp::endpoint ep)
                        -> asio::awaitable<void, executor_type> {
                        co_await c.async_connect(ep, asio::deferred);
                    }(client, ep),
                    asio::detached);

                server = co_await acc.async_accept(asio::deferred);

                char byte = 'X';
                co_await asio::async_write(
                    client, asio::buffer(&byte, 1), asio::deferred);

                char recv = 0;
                co_await asio::async_read(
                    server, asio::buffer(&recv, 1), asio::deferred);

                client.close();
                server.close();
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    for (int i = 0; i < num_loops; ++i)
        asio::co_spawn(ioc, loop_task(i), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    for (auto& a : acceptors)
        a.close();
}

// Burst N connects then accept all
void
bench_burst_churn(bench::state& state)
{
    int burst_size = static_cast<int>(state.range(0));
    state.counters["burst_size"] = burst_size;

    asio::io_context ioc;
    auto acc = make_churn_acceptor(ioc);
    auto ep  = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto lp = state.lap();

                std::vector<tcp_socket> clients;
                std::vector<tcp_socket> servers;
                clients.reserve(burst_size);
                servers.reserve(burst_size);

                bool open_ok = true;
                for (int i = 0; i < burst_size; ++i)
                {
                    clients.emplace_back(ioc.get_executor());
                    boost::system::error_code ec;
                    ec = clients.back().open(tcp::v4(), ec);
                    if (ec)
                    {
                        clients.clear();
                        open_ok = false;
                        break;
                    }
                    configure_churn_socket(clients.back());
                }
                if (!open_ok)
                    continue;

                for (int i = 0; i < burst_size; ++i)
                {
                    asio::co_spawn(
                        ioc,
                        [](tcp_socket& c, tcp::endpoint ep)
                            -> asio::awaitable<void, executor_type> {
                            co_await c.async_connect(ep, asio::deferred);
                        }(clients[i], ep),
                        asio::detached);
                }

                for (int i = 0; i < burst_size; ++i)
                {
                    servers.push_back(
                        co_await acc.async_accept(asio::deferred));
                }

                for (auto& c : clients)
                    c.close();
                for (auto& s : servers)
                    s.close();
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

void
bench_burst_churn_lockless(bench::state& state)
{
    int burst_size = static_cast<int>(state.range(0));
    state.counters["burst_size"] = burst_size;

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto acc = make_churn_acceptor(ioc);
    auto ep  = tcp::endpoint(
        asio::ip::address_v4::loopback(), acc.local_endpoint().port());

    std::atomic<bool> running{true};

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto lp = state.lap();

                std::vector<tcp_socket> clients;
                std::vector<tcp_socket> servers;
                clients.reserve(burst_size);
                servers.reserve(burst_size);

                bool open_ok = true;
                for (int i = 0; i < burst_size; ++i)
                {
                    clients.emplace_back(ioc.get_executor());
                    boost::system::error_code ec;
                    ec = clients.back().open(tcp::v4(), ec);
                    if (ec)
                    {
                        clients.clear();
                        open_ok = false;
                        break;
                    }
                    configure_churn_socket(clients.back());
                }
                if (!open_ok)
                    continue;

                for (int i = 0; i < burst_size; ++i)
                {
                    asio::co_spawn(
                        ioc,
                        [](tcp_socket& c, tcp::endpoint ep)
                            -> asio::awaitable<void, executor_type> {
                            co_await c.async_connect(ep, asio::deferred);
                        }(clients[i], ep),
                        asio::detached);
                }

                for (int i = 0; i < burst_size; ++i)
                {
                    servers.push_back(
                        co_await acc.async_accept(asio::deferred));
                }

                for (auto& c : clients)
                    c.close();
                for (auto& s : servers)
                    s.close();
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
    acc.close();
}

} // anonymous namespace

bench::benchmark_suite
make_accept_churn_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("accept_churn", F::needs_conntrack_drain)
        .add("sequential", bench_sequential_churn)
        .add("sequential_lockless", bench_sequential_churn_lockless)
        .add("concurrent", bench_concurrent_churn)
            .args({1, 4, 16})
        .add("burst", bench_burst_churn)
            .args({10, 100})
        .add("burst_lockless", bench_burst_churn_lockless)
            .args({10, 100});
}

} // namespace asio_bench
