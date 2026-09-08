//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include "../local_socket_utils.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using asio_bench::local_socket;

namespace asio_callback_bench {
namespace {

struct unix_write_op
{
    local_socket& sock;
    std::vector<char>& buf;
    std::size_t chunk_size;
    std::atomic<bool>& running;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
        {
            sock.shutdown(local_socket::shutdown_send);
            return;
        }
        sock.async_write_some(
            asio::buffer(buf.data(), chunk_size),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                start();
            });
    }
};

struct unix_read_op
{
    local_socket& sock;
    std::vector<char>& buf;
    std::size_t& total_read;

    void start()
    {
        sock.async_read_some(
            asio::buffer(buf.data(), buf.size()),
            [this](boost::system::error_code ec, std::size_t n) {
                if (ec || n == 0)
                    return;
                total_read += n;
                start();
            });
    }
};

void
bench_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc;
    auto [writer, reader] = asio_bench::make_local_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_read = 0;

    unix_write_op wop{writer, write_buf, chunk_size, running};
    unix_read_op rop{reader, read_buf, total_read};

    perf::stopwatch sw;

    wop.start();
    rop.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(static_cast<int64_t>(total_read));

    writer.close();
    reader.close();
}

void
bench_bidirectional_throughput(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc;
    auto [sock1, sock2] = asio_bench::make_local_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');
    std::vector<char> rbuf1(chunk_size);
    std::vector<char> rbuf2(chunk_size);

    std::atomic<bool> running{true};
    std::size_t read1 = 0;
    std::size_t read2 = 0;

    unix_write_op wop1{sock1, buf1, chunk_size, running};
    unix_read_op rop1{sock2, rbuf1, read1};

    unix_write_op wop2{sock2, buf2, chunk_size, running};
    unix_read_op rop2{sock1, rbuf2, read2};

    perf::stopwatch sw;

    wop1.start();
    rop1.start();
    wop2.start();
    rop2.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(static_cast<int64_t>(read1 + read2));

    sock1.close();
    sock2.close();
}

void
bench_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto [writer, reader] = asio_bench::make_local_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_read = 0;

    unix_write_op wop{writer, write_buf, chunk_size, running};
    unix_read_op rop{reader, read_buf, total_read};

    perf::stopwatch sw;

    wop.start();
    rop.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(static_cast<int64_t>(total_read));

    writer.close();
    reader.close();
}

void
bench_bidirectional_throughput_lockless(bench::state& state)
{
    auto chunk_size = static_cast<std::size_t>(state.range(0));
    state.counters["chunk_size"] = static_cast<double>(chunk_size);

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    auto [sock1, sock2] = asio_bench::make_local_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');
    std::vector<char> rbuf1(chunk_size);
    std::vector<char> rbuf2(chunk_size);

    std::atomic<bool> running{true};
    std::size_t read1 = 0;
    std::size_t read2 = 0;

    unix_write_op wop1{sock1, buf1, chunk_size, running};
    unix_read_op rop1{sock2, rbuf1, read1};

    unix_write_op wop2{sock2, buf2, chunk_size, running};
    unix_read_op rop2{sock1, rbuf2, read2};

    perf::stopwatch sw;

    wop1.start();
    rop1.start();
    wop2.start();
    rop2.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(static_cast<int64_t>(read1 + read2));

    sock1.close();
    sock2.close();
}

} // anonymous namespace

bench::benchmark_suite
make_local_socket_throughput_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("local_socket_throughput", F::none)
        .add("unidirectional", bench_throughput)
            .range(1024, 1048576, 4)
        .add("unidirectional_lockless", bench_throughput_lockless)
            .range(1024, 1048576, 4)
        .add("bidirectional", bench_bidirectional_throughput)
            .range(1024, 1048576, 4)
        .add("bidirectional_lockless", bench_bidirectional_throughput_lockless)
            .range(1024, 1048576, 4);
}

} // namespace asio_callback_bench
