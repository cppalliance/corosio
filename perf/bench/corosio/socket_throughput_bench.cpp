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
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#if BOOST_COROSIO_HAS_IOCP
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

inline void
set_nodelay(corosio::tcp_socket& s)
{
    int flag = 1;
#if BOOST_COROSIO_HAS_IOCP
    ::setsockopt(
        static_cast<SOCKET>(s.native_handle()), IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<char const*>(&flag), sizeof(flag));
#else
    ::setsockopt(
        s.native_handle(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
}

template<auto Backend>
void
bench_throughput(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::native_io_context<Backend> ioc;
    auto [writer, reader] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    set_nodelay(writer);
    set_nodelay(reader);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};

    auto write_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), chunk_size));
            if (ec)
                break;
        }
        writer.shutdown(corosio::tcp_socket::shutdown_send);
    };

    auto read_task = [&]() -> capy::task<> {
        for (;;)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec || n == 0)
                break;
            state.add_bytes(static_cast<int64_t>(n));
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(write_task());
    capy::run_async(ioc.get_executor())(read_task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    writer.close();
    reader.close();
}

template<auto Backend>
void
bench_bidirectional_throughput(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::native_io_context<Backend> ioc;
    auto [sock1, sock2] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    set_nodelay(sock1);
    set_nodelay(sock2);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};

    auto write1_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), chunk_size));
            if (ec)
                break;
        }
        sock1.shutdown(corosio::tcp_socket::shutdown_send);
    };

    auto read1_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            state.add_bytes(static_cast<int64_t>(n));
        }
    };

    auto write2_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock2.write_some(
                capy::const_buffer(buf2.data(), chunk_size));
            if (ec)
                break;
        }
        sock2.shutdown(corosio::tcp_socket::shutdown_send);
    };

    auto read2_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            state.add_bytes(static_cast<int64_t>(n));
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(write1_task());
    capy::run_async(ioc.get_executor())(read1_task());
    capy::run_async(ioc.get_executor())(write2_task());
    capy::run_async(ioc.get_executor())(read2_task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    sock1.close();
    sock2.close();
}

template<auto Backend>
capy::task<>
mt_write_coro(
    corosio::native_tcp_socket<Backend>& sock,
    std::vector<char>& wbuf,
    std::size_t chunk_size,
    std::atomic<bool>& running)
{
    while (running.load(std::memory_order_relaxed))
    {
        auto [ec, n] = co_await sock.write_some(
            capy::const_buffer(wbuf.data(), chunk_size));
        if (ec)
            break;
    }
    sock.shutdown(corosio::tcp_socket::shutdown_send);
}

template<auto Backend>
capy::task<>
mt_read_coro(
    corosio::native_tcp_socket<Backend>& sock,
    std::size_t chunk_size,
    bench::state& state)
{
    std::vector<char> rbuf(chunk_size);
    for (;;)
    {
        auto [ec, n] = co_await sock.read_some(
            capy::mutable_buffer(rbuf.data(), rbuf.size()));
        if (ec || n == 0)
            break;
        state.add_bytes(static_cast<int64_t>(n));
    }
}

template<auto Backend>
void
bench_multithread_throughput(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    int num_threads     = static_cast<int>(state.range(0));
    int num_connections = 32;
    auto chunk_size     = static_cast<std::size_t>(65536);

    state.counters["threads"]     = num_threads;
    state.counters["connections"] = num_connections;
    state.counters["chunk_size"]  = static_cast<double>(chunk_size);

    corosio::native_io_context<Backend> ioc;

    struct pair_bufs
    {
        std::vector<char> wbuf1;
        std::vector<char> wbuf2;
    };

    std::vector<socket_type> sock1s;
    std::vector<socket_type> sock2s;
    std::vector<pair_bufs> bufs;

    sock1s.reserve(num_connections);
    sock2s.reserve(num_connections);
    bufs.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [s1, s2] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        set_nodelay(s1);
        set_nodelay(s2);
        sock1s.push_back(std::move(s1));
        sock2s.push_back(std::move(s2));
        bufs.push_back(
            {std::vector<char>(chunk_size, 'a'),
             std::vector<char>(chunk_size, 'b')});
    }

    std::atomic<bool> running{true};

    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(mt_write_coro<Backend>(
            sock1s[i], bufs[i].wbuf1, chunk_size, running));
        capy::run_async(ioc.get_executor())(
            mt_read_coro<Backend>(sock2s[i], chunk_size, state));
        capy::run_async(ioc.get_executor())(mt_write_coro<Backend>(
            sock2s[i], bufs[i].wbuf2, chunk_size, running));
        capy::run_async(ioc.get_executor())(
            mt_read_coro<Backend>(sock1s[i], chunk_size, state));
    }

    perf::stopwatch sw;

    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int i = 1; i < num_threads; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();

    timer.join();
    for (auto& t : threads)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());

    for (auto& s : sock1s)
        s.close();
    for (auto& s : sock2s)
        s.close();
}

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_socket_throughput_suite()
{
    using F      = bench::bench_flags;
    using socket_type = corosio::native_tcp_socket<Backend>;

    return bench::benchmark_suite("socket_throughput", F::needs_conntrack_drain)
        .set_warmup([]{
            corosio::native_io_context<Backend> ioc;
            auto [w, r] = corosio::test::make_socket_pair<
                socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
            std::vector<char> buf(4096, 'w');
            auto task = [&]() -> capy::task<> {
                (void)co_await w.write_some(
                    capy::const_buffer(buf.data(), buf.size()));
                (void)co_await r.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
            };
            capy::run_async(ioc.get_executor())(task());
            ioc.run();
            w.close();
            r.close();
        })
        .add("unidirectional", bench_throughput<Backend>)
            .range(1024, 1048576, 4)
        .add("bidirectional", bench_bidirectional_throughput<Backend>)
            .range(1024, 1048576, 4)
        .add("multithread", bench_multithread_throughput<Backend>)
            .args({2, 4, 8});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_socket_throughput_suite)
