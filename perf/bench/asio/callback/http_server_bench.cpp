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
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../common/http_protocol.hpp"

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

// Two-phase server loop: read request headers, write response
struct server_op
{
    tcp_socket& sock;
    std::string buf;

    void start()
    {
        do_read();
    }

    void do_read()
    {
        asio::async_read_until(
            sock, asio::dynamic_buffer(buf), "\r\n\r\n",
            [this](boost::system::error_code ec, std::size_t n) {
                if (ec)
                    return;
                do_write(n);
            });
    }

    void do_write(std::size_t consumed)
    {
        asio::async_write(
            sock,
            asio::buffer(
                bench::http::small_response, bench::http::small_response_size),
            [this, consumed](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                buf.erase(0, consumed);
                do_read();
            });
    }
};

// Three-phase client loop: write request, read headers, optionally read body
struct client_op
{
    tcp_socket& sock;
    bench::state& state;
    std::string buf;
    perf::stopwatch sw;

    void start()
    {
        if (!state.running())
        {
            sock.shutdown(tcp_socket::shutdown_send);
            return;
        }
        sw.reset();
        do_write();
    }

    void do_write()
    {
        asio::async_write(
            sock,
            asio::buffer(
                bench::http::small_request, bench::http::small_request_size),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                do_read_headers();
            });
    }

    void do_read_headers()
    {
        asio::async_read_until(
            sock, asio::dynamic_buffer(buf), "\r\n\r\n",
            [this](boost::system::error_code ec, std::size_t header_end) {
                if (ec)
                    return;

                std::string_view headers(buf.data(), header_end);
                std::size_t content_length = 0;
                auto pos                   = headers.find("Content-Length: ");
                if (pos != std::string_view::npos)
                {
                    pos += 16;
                    while (pos < headers.size() && headers[pos] >= '0' &&
                           headers[pos] <= '9')
                    {
                        content_length =
                            content_length * 10 + (headers[pos] - '0');
                        ++pos;
                    }
                }

                std::size_t total_size = header_end + content_length;
                if (buf.size() < total_size)
                    do_read_body(total_size);
                else
                    finish_request(total_size);
            });
    }

    void do_read_body(std::size_t total_size)
    {
        std::size_t need     = total_size - buf.size();
        std::size_t old_size = buf.size();
        buf.resize(total_size);
        asio::async_read(
            sock, asio::buffer(buf.data() + old_size, need),
            [this, total_size](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                finish_request(total_size);
            });
    }

    void finish_request(std::size_t total_size)
    {
        state.record_latency(sw.elapsed_ns());
        state.ops().fetch_add(1, std::memory_order_relaxed);
        buf.erase(0, total_size);
        start();
    }
};

void
bench_single_connection(bench::state& state)
{
    asio::io_context ioc;
    auto [client, server] = asio_bench::make_socket_pair(ioc);

    server_op sop{server, {}};
    client_op cop{client, state, {}, {}};

    sop.start();
    cop.start();

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
    auto [client, server] = asio_bench::make_socket_pair(ioc);

    server_op sop{server, {}};
    client_op cop{client, state, {}, {}};

    sop.start();
    cop.start();

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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    std::vector<std::unique_ptr<server_op>> sops;
    std::vector<std::unique_ptr<client_op>> cops;
    sops.reserve(num_connections);
    cops.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        sops.push_back(
            std::make_unique<server_op>(server_op{servers[i], {}}));
        cops.push_back(
            std::make_unique<client_op>(
                client_op{clients[i], state, {}, {}}));
        sops.back()->start();
        cops.back()->start();
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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    std::vector<std::unique_ptr<server_op>> sops;
    std::vector<std::unique_ptr<client_op>> cops;
    sops.reserve(num_connections);
    cops.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        sops.push_back(
            std::make_unique<server_op>(server_op{servers[i], {}}));
        cops.push_back(
            std::make_unique<client_op>(
                client_op{clients[i], state, {}, {}}));
        sops.back()->start();
        cops.back()->start();
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
        .add("single_conn", bench_single_connection)
        .add("single_conn_lockless", bench_single_connection_lockless)
        .add("concurrent", bench_concurrent_connections)
            .args({1, 4, 16, 32})
        .add("multithread", bench_multithread)
            .args({1, 2, 4, 8, 16});
}

} // namespace asio_callback_bench
