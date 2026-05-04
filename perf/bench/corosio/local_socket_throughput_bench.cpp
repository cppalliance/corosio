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
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

template<auto Backend>
void
bench_unix_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::native_io_context<Backend> ioc;
    auto [writer, reader] = corosio::make_local_stream_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    int64_t total_bytes = 0;

    auto write_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), chunk_size));
            if (ec)
                break;
        }
        writer.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read_task = [&]() -> capy::task<> {
        for (;;)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec || n == 0)
                break;
            total_bytes += static_cast<int64_t>(n);
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
    state.add_bytes(total_bytes);
    writer.close();
    reader.close();
}

template<auto Backend>
void
bench_unix_bidirectional_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::native_io_context<Backend> ioc;
    auto [sock1, sock2] = corosio::make_local_stream_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    int64_t read1_bytes = 0;
    int64_t read2_bytes = 0;

    auto write1_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), chunk_size));
            if (ec)
                break;
        }
        sock1.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read1_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read1_bytes += static_cast<int64_t>(n);
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
        sock2.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read2_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read2_bytes += static_cast<int64_t>(n);
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
    state.add_bytes(read1_bytes + read2_bytes);
    sock1.close();
    sock2.close();
}

template<auto Backend>
void
bench_unix_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts, 1);
    auto [writer, reader] = corosio::make_local_stream_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    int64_t total_bytes = 0;

    auto write_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), chunk_size));
            if (ec)
                break;
        }
        writer.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read_task = [&]() -> capy::task<> {
        for (;;)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec || n == 0)
                break;
            total_bytes += static_cast<int64_t>(n);
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
    state.add_bytes(total_bytes);
    writer.close();
    reader.close();
}

template<auto Backend>
void
bench_unix_bidirectional_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts, 1);
    auto [sock1, sock2] = corosio::make_local_stream_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    int64_t read1_bytes = 0;
    int64_t read2_bytes = 0;

    auto write1_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), chunk_size));
            if (ec)
                break;
        }
        sock1.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read1_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read1_bytes += static_cast<int64_t>(n);
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
        sock2.shutdown(corosio::local_stream_socket::shutdown_send);
    };

    auto read2_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read2_bytes += static_cast<int64_t>(n);
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
    state.add_bytes(read1_bytes + read2_bytes);
    sock1.close();
    sock2.close();
}

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_local_socket_throughput_suite()
{
    using F = bench::bench_flags;

    return bench::benchmark_suite("local_socket_throughput", F::none)
        .add("unidirectional", bench_unix_throughput<Backend>)
            .range(1024, 1048576, 4)
        .add("unidirectional_lockless", bench_unix_throughput_lockless<Backend>)
            .range(1024, 1048576, 4)
        .add("bidirectional", bench_unix_bidirectional_throughput<Backend>)
            .range(1024, 1048576, 4)
        .add("bidirectional_lockless", bench_unix_bidirectional_throughput_lockless<Backend>)
            .range(1024, 1048576, 4);
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE_POSIX(corosio_bench::make_local_socket_throughput_suite)

#endif // BOOST_COROSIO_POSIX
