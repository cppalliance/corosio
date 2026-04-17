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
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../../common/http_protocol.hpp"

namespace asio_bench {
namespace {

// Server: loop until read error (EOF from client shutdown)
asio::awaitable<void, executor_type>
server_task(tcp_socket& sock)
{
    std::string buf;

    try
    {
        for (;;)
        {
            std::size_t n = co_await asio::async_read_until(
                sock, asio::dynamic_buffer(buf), "\r\n\r\n", asio::deferred);

            co_await asio::async_write(
                sock,
                asio::buffer(
                    bench::http::small_response,
                    bench::http::small_response_size),
                asio::deferred);

            buf.erase(0, n);
        }
    }
    catch (std::exception const&)
    {
    }
}

// Client: loop while running, then shutdown
asio::awaitable<void, executor_type>
client_task(tcp_socket& sock, bench::state& state)
{
    std::string buf;

    try
    {
        while (state.running())
        {
            auto lp = state.lap();

            co_await asio::async_write(
                sock,
                asio::buffer(
                    bench::http::small_request,
                    bench::http::small_request_size),
                asio::deferred);

            std::size_t header_end = co_await asio::async_read_until(
                sock, asio::dynamic_buffer(buf), "\r\n\r\n", asio::deferred);

            std::string_view headers(buf.data(), header_end);
            std::size_t content_length = 0;
            auto pos                   = headers.find("Content-Length: ");
            if (pos != std::string_view::npos)
            {
                pos += 16;
                while (pos < headers.size() && headers[pos] >= '0' &&
                       headers[pos] <= '9')
                {
                    content_length = content_length * 10 + (headers[pos] - '0');
                    ++pos;
                }
            }

            std::size_t total_size = header_end + content_length;
            if (buf.size() < total_size)
            {
                std::size_t need     = total_size - buf.size();
                std::size_t old_size = buf.size();
                buf.resize(total_size);
                co_await asio::async_read(
                    sock, asio::buffer(buf.data() + old_size, need),
                    asio::deferred);
            }

            buf.erase(0, total_size);
        }

        sock.shutdown(tcp_socket::shutdown_send);
    }
    catch (std::exception const&)
    {
    }
}

void
bench_single_connection(bench::state& state)
{
    asio::io_context ioc;
    auto [client, server] = make_socket_pair(ioc);

    asio::co_spawn(
        ioc, server_task(server), asio::detached);
    asio::co_spawn(
        ioc, client_task(client, state), asio::detached);

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
bench_single_connection_lockless(bench::state& state)
{
    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto [client, server] = make_socket_pair(ioc);

    asio::co_spawn(
        ioc, server_task(server), asio::detached);
    asio::co_spawn(
        ioc, client_task(client, state), asio::detached);

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
bench_concurrent_connections(bench::state& state)
{
    int num_connections = static_cast<int>(state.range(0));
    state.counters["connections"] = num_connections;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < num_connections; ++i)
    {
        asio::co_spawn(
            ioc, server_task(servers[i]), asio::detached);
        asio::co_spawn(
            ioc, client_task(clients[i], state), asio::detached);
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

void
bench_multithread(bench::state& state)
{
    int num_threads     = static_cast<int>(state.range(0));
    int num_connections = 32;

    state.counters["threads"]     = num_threads;
    state.counters["connections"] = num_connections;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < num_connections; ++i)
    {
        asio::co_spawn(
            ioc, server_task(servers[i]), asio::detached);
        asio::co_spawn(
            ioc, client_task(clients[i], state), asio::detached);
    }

    perf::stopwatch sw;

    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int i = 1; i < num_threads; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();

    timer.join();
    for (auto& t : threads)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();
}

} // anonymous namespace

bench::benchmark_suite
make_http_server_suite()
{
    using F = bench::bench_flags;

    return bench::benchmark_suite("http_server", F::needs_conntrack_drain)
        .set_warmup([]{
            asio::io_context ioc;
            auto [c, s]   = make_socket_pair(ioc);
            char buf[256] = {};
            for (int i = 0; i < 10; ++i)
            {
                asio::write(
                    c,
                    asio::buffer(
                        bench::http::small_request,
                        bench::http::small_request_size));
                asio::read(s, asio::buffer(buf, bench::http::small_request_size));
                asio::write(
                    s,
                    asio::buffer(
                        bench::http::small_response,
                        bench::http::small_response_size));
                asio::read(c, asio::buffer(buf, bench::http::small_response_size));
            }
            c.close();
            s.close();
        })
        .add("single_conn", bench_single_connection)
        .add("single_conn_lockless", bench_single_connection_lockless)
        .add("concurrent", bench_concurrent_connections)
            .args({1, 4, 16, 32})
        .add("multithread", bench_multithread)
            .args({1, 2, 4, 8, 16});
}

} // namespace asio_bench
