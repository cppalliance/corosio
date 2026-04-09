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
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/buffer.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio_bench {
namespace {

void
bench_throughput_impl(bench::state& state, bool lockless)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(lockless ? BOOST_ASIO_CONCURRENCY_HINT_UNSAFE : 1);
    auto [writer, reader] = make_unix_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};

    auto write_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await writer.async_write_some(
                    asio::buffer(write_buf.data(), chunk_size), asio::deferred);
            }
            writer.shutdown(local_socket::shutdown_send);
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
                state.add_bytes(static_cast<int64_t>(n));
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
    writer.close();
    reader.close();
}

void bench_throughput(bench::state& s) { bench_throughput_impl(s, false); }
void bench_throughput_lockless(bench::state& s) { bench_throughput_impl(s, true); }

void
bench_bidirectional_throughput_impl(bench::state& state, bool lockless)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(lockless ? BOOST_ASIO_CONCURRENCY_HINT_UNSAFE : 1);
    auto [sock1, sock2] = make_unix_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};

    auto write1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                co_await sock1.async_write_some(
                    asio::buffer(buf1.data(), chunk_size), asio::deferred);
            }
            sock1.shutdown(local_socket::shutdown_send);
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
                state.add_bytes(static_cast<int64_t>(n));
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
            sock2.shutdown(local_socket::shutdown_send);
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
                state.add_bytes(static_cast<int64_t>(n));
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
    sock1.close();
    sock2.close();
}

void bench_bidirectional_throughput(bench::state& s) { bench_bidirectional_throughput_impl(s, false); }
void bench_bidirectional_throughput_lockless(bench::state& s) { bench_bidirectional_throughput_impl(s, true); }

} // anonymous namespace

bench::benchmark_suite
make_unix_socket_throughput_suite()
{
    return bench::benchmark_suite("unix_socket_throughput")
        .set_warmup([] {
            asio::io_context ioc;
            auto [w, r] = make_unix_socket_pair(ioc);
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
            .range(1024, 1048576, 4);
}

} // namespace asio_bench
