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
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio_bench {
namespace {

void
bench_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc;
    auto [writer, reader] = make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    int64_t total_bytes = 0;

    auto write_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await writer.async_write_some(
                    asio::buffer(write_buf.data(), chunk_size), asio::deferred);
            }
            writer.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            for (;;)
            {
                auto n = co_await reader.async_read_some(
                    asio::buffer(read_buf.data(), read_buf.size()),
                    asio::deferred);
                if (n == 0)
                    break;
                total_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, write_task(), asio::detached);
    asio::co_spawn(ioc, read_task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(total_bytes);
    writer.close();
    reader.close();
}

void
bench_bidirectional_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc;
    auto [sock1, sock2] = make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    int64_t read1_bytes = 0;
    int64_t read2_bytes = 0;

    auto write1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await sock1.async_write_some(
                    asio::buffer(buf1.data(), chunk_size), asio::deferred);
            }
            sock1.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            std::vector<char> rbuf(chunk_size);
            for (;;)
            {
                auto n = co_await sock2.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()), asio::deferred);
                if (n == 0)
                    break;
                read1_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    auto write2_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await sock2.async_write_some(
                    asio::buffer(buf2.data(), chunk_size), asio::deferred);
            }
            sock2.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read2_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            std::vector<char> rbuf(chunk_size);
            for (;;)
            {
                auto n = co_await sock1.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()), asio::deferred);
                if (n == 0)
                    break;
                read2_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, write1_task(), asio::detached);
    asio::co_spawn(ioc, read1_task(), asio::detached);
    asio::co_spawn(ioc, write2_task(), asio::detached);
    asio::co_spawn(ioc, read2_task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(read1_bytes + read2_bytes);
    sock1.close();
    sock2.close();
}

void
bench_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto [writer, reader] = make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    int64_t total_bytes = 0;

    auto write_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await writer.async_write_some(
                    asio::buffer(write_buf.data(), chunk_size), asio::deferred);
            }
            writer.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            for (;;)
            {
                auto n = co_await reader.async_read_some(
                    asio::buffer(read_buf.data(), read_buf.size()),
                    asio::deferred);
                if (n == 0)
                    break;
                total_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, write_task(), asio::detached);
    asio::co_spawn(ioc, read_task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(total_bytes);
    writer.close();
    reader.close();
}

void
bench_bidirectional_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto [sock1, sock2] = make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    int64_t read1_bytes = 0;
    int64_t read2_bytes = 0;

    auto write1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await sock1.async_write_some(
                    asio::buffer(buf1.data(), chunk_size), asio::deferred);
            }
            sock1.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            std::vector<char> rbuf(chunk_size);
            for (;;)
            {
                auto n = co_await sock2.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()), asio::deferred);
                if (n == 0)
                    break;
                read1_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    auto write2_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await sock2.async_write_some(
                    asio::buffer(buf2.data(), chunk_size), asio::deferred);
            }
            sock2.shutdown(tcp_socket::shutdown_send);
        }
        catch (std::exception const&)
        {
        }
    };

    auto read2_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            std::vector<char> rbuf(chunk_size);
            for (;;)
            {
                auto n = co_await sock1.async_read_some(
                    asio::buffer(rbuf.data(), rbuf.size()), asio::deferred);
                if (n == 0)
                    break;
                read2_bytes += static_cast<int64_t>(n);
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, write1_task(), asio::detached);
    asio::co_spawn(ioc, read1_task(), asio::detached);
    asio::co_spawn(ioc, write2_task(), asio::detached);
    asio::co_spawn(ioc, read2_task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(read1_bytes + read2_bytes);
    sock1.close();
    sock2.close();
}

// Free coroutine functions avoid dangling-this when spawned in a loop
asio::awaitable<void, executor_type>
mt_write_coro(
    tcp_socket& sock,
    std::vector<char>& wbuf,
    std::size_t chunk_size,
    std::atomic<bool>& running)
{
    try
    {
        while (running.load(std::memory_order_relaxed))
        {
            co_await sock.async_write_some(
                asio::buffer(wbuf.data(), chunk_size), asio::deferred);
        }
        sock.shutdown(tcp_socket::shutdown_send);
    }
    catch (std::exception const&)
    {
    }
}

asio::awaitable<void, executor_type>
mt_read_coro(
    tcp_socket& sock,
    std::size_t chunk_size,
    bench::state& state)
{
    try
    {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto n = co_await sock.async_read_some(
                asio::buffer(rbuf.data(), rbuf.size()), asio::deferred);
            if (n == 0)
                break;
            state.add_bytes(static_cast<int64_t>(n));
        }
    }
    catch (std::exception const&)
    {
    }
}

void
bench_multithread_throughput(bench::state& state)
{
    int num_threads     = static_cast<int>(state.range(0));
    int num_connections = 32;
    auto chunk_size     = static_cast<std::size_t>(65536);

    state.counters["threads"]     = num_threads;
    state.counters["connections"] = num_connections;
    state.counters["chunk_size"]  = static_cast<double>(chunk_size);

    asio::io_context ioc;

    struct pair_bufs
    {
        std::vector<char> wbuf1;
        std::vector<char> wbuf2;
    };

    std::vector<tcp_socket> sock1s;
    std::vector<tcp_socket> sock2s;
    std::vector<pair_bufs> bufs;

    sock1s.reserve(num_connections);
    sock2s.reserve(num_connections);
    bufs.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [s1, s2] = make_socket_pair(ioc);
        sock1s.push_back(std::move(s1));
        sock2s.push_back(std::move(s2));
        bufs.push_back(
            {std::vector<char>(chunk_size, 'a'),
             std::vector<char>(chunk_size, 'b')});
    }

    std::atomic<bool> running{true};

    for (int i = 0; i < num_connections; ++i)
    {
        asio::co_spawn(
            ioc, mt_write_coro(sock1s[i], bufs[i].wbuf1, chunk_size, running),
            asio::detached);
        asio::co_spawn(
            ioc, mt_read_coro(sock2s[i], chunk_size, state),
            asio::detached);
        asio::co_spawn(
            ioc, mt_write_coro(sock2s[i], bufs[i].wbuf2, chunk_size, running),
            asio::detached);
        asio::co_spawn(
            ioc, mt_read_coro(sock1s[i], chunk_size, state),
            asio::detached);
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

bench::benchmark_suite
make_socket_throughput_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("socket_throughput", F::needs_conntrack_drain)
        .set_warmup([] {
            asio::io_context ioc;
            auto [w, r] = make_socket_pair(ioc);
            std::vector<char> buf(4096, 'w');
            asio::write(w, asio::buffer(buf));
            asio::read(r, asio::buffer(buf));
            w.close();
            r.close();
        })
        .add("unidirectional", bench_throughput)
            .range(1024, 1048576, 4)
        .add("unidirectional_lockless", bench_throughput_lockless)
            .range(1024, 1048576, 4)
        .add("bidirectional", bench_bidirectional_throughput)
            .range(1024, 1048576, 4)
        .add("bidirectional_lockless", bench_bidirectional_throughput_lockless)
            .range(1024, 1048576, 4)
        .add("multithread", bench_multithread_throughput)
            .args({2, 4, 8});
}

} // namespace asio_bench
