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
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../common/http_protocol.hpp"
#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

// Read into `buf` until `delim` appears; return the index just past it.
// capy dropped composed read_until, and this benchmark only needs to locate
// the header terminator; bytes past the delimiter stay buffered in `buf`.
template<class Sock>
capy::task<std::pair<std::error_code, std::size_t>>
read_until(Sock& sock, std::string& buf, std::string_view delim)
{
    std::size_t scan_from = 0;
    for (;;)
    {
        if (auto pos = buf.find(delim, scan_from); pos != std::string::npos)
            co_return {std::error_code{}, pos + delim.size()};
        // A full delimiter can only straddle the last delim.size()-1 bytes,
        // so the next scan need not re-examine earlier bytes.
        scan_from =
            buf.size() >= delim.size() ? buf.size() - (delim.size() - 1) : 0;
        char chunk[4096];
        auto [ec, n] =
            co_await sock.read_some(capy::mutable_buffer(chunk, sizeof(chunk)));
        buf.append(chunk, n);
        if (ec)
            co_return {ec, buf.size()};
    }
}

template<auto Backend>
capy::task<>
server_task(corosio::native_tcp_socket<Backend>& sock)
{
    std::string buf;

    for (;;)
    {
        auto [ec, n] = co_await read_until(sock, buf, "\r\n\r\n");
        if (ec)
            co_return;

        auto [wec, wn] = co_await capy::write(
            sock,
            capy::const_buffer(
                bench::http::small_response, bench::http::small_response_size));
        if (wec)
            co_return;

        buf.erase(0, n);
    }
}

template<auto Backend>
capy::task<>
client_task(
    corosio::native_tcp_socket<Backend>& sock,
    bench::state& state)
{
    std::string buf;

    while (state.running())
    {
        auto lp = state.lap();

        auto [wec, wn] = co_await capy::write(
            sock,
            capy::const_buffer(
                bench::http::small_request, bench::http::small_request_size));
        if (wec)
            co_return;

        auto [ec, header_end] = co_await read_until(sock, buf, "\r\n\r\n");
        if (ec)
            co_return;

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
            auto [rec, rn] = co_await capy::read(
                sock, capy::mutable_buffer(buf.data() + old_size, need));
            if (rec)
                co_return;
        }

        buf.erase(0, total_size);
    }

    sock.shutdown(corosio::tcp_socket::shutdown_send);
}

template<auto Backend>
void
bench_single_connection(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    corosio::native_io_context<Backend> ioc;
    auto [client, server] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    client.set_option(corosio::native_socket_option::no_delay(true));
    server.set_option(corosio::native_socket_option::no_delay(true));

    capy::run_async(ioc.get_executor())(server_task<Backend>(server));
    capy::run_async(ioc.get_executor())(client_task<Backend>(client, state));

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
bench_single_connection_lockless(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    corosio::io_context_options opts;
    opts.locking = corosio::locking_mode::unsafe;
    corosio::native_io_context<Backend> ioc(opts, 1);
    auto [client, server] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    client.set_option(corosio::native_socket_option::no_delay(true));
    server.set_option(corosio::native_socket_option::no_delay(true));

    capy::run_async(ioc.get_executor())(server_task<Backend>(server));
    capy::run_async(ioc.get_executor())(client_task<Backend>(client, state));

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
bench_concurrent_connections(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    int num_connections = static_cast<int>(state.range(0));
    state.counters["connections"] = num_connections;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(
            server_task<Backend>(servers[i]));
        capy::run_async(ioc.get_executor())(
            client_task<Backend>(clients[i], state));
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
bench_multithread(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    int num_threads     = static_cast<int>(state.range(0));
    int num_connections = 32;

    state.counters["threads"]     = num_threads;
    state.counters["connections"] = num_connections;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(
            server_task<Backend>(servers[i]));
        capy::run_async(ioc.get_executor())(
            client_task<Backend>(clients[i], state));
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

template<auto Backend>
bench::benchmark_suite
make_http_server_suite()
{
    using F = bench::bench_flags;

    return bench::benchmark_suite("http_server", F::needs_conntrack_drain)
        .add("single_conn", bench_single_connection<Backend>)
        .add("single_conn_lockless", bench_single_connection_lockless<Backend>)
        .add("concurrent", bench_concurrent_connections<Backend>)
            .args({1, 4, 16, 32})
        .add("multithread", bench_multithread<Backend>)
            .args({1, 2, 4, 8, 16});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_http_server_suite)
