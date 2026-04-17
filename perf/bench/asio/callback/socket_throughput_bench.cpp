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
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

struct write_op
{
    tcp_socket& sock;
    std::vector<char>& buf;
    std::size_t chunk_size;
    std::atomic<bool>& running;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
        {
            sock.shutdown(tcp_socket::shutdown_send);
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

struct read_op
{
    tcp_socket& sock;
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
    auto [writer, reader] = asio_bench::make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_read = 0;

    write_op wop{writer, write_buf, chunk_size, running};
    read_op rop{reader, read_buf, total_read};

    perf::stopwatch sw;

    wop.start();
    rop.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
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
    auto [sock1, sock2] = asio_bench::make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');
    std::vector<char> rbuf1(chunk_size);
    std::vector<char> rbuf2(chunk_size);

    std::atomic<bool> running{true};
    std::size_t read1 = 0;
    std::size_t read2 = 0;

    // sock1 writes, sock2 reads (direction 1)
    write_op wop1{sock1, buf1, chunk_size, running};
    read_op rop1{sock2, rbuf1, read1};

    // sock2 writes, sock1 reads (direction 2)
    write_op wop2{sock2, buf2, chunk_size, running};
    read_op rop2{sock1, rbuf2, read2};

    perf::stopwatch sw;

    wop1.start();
    rop1.start();
    wop2.start();
    rop2.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
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
    auto [writer, reader] = asio_bench::make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_read = 0;

    write_op wop{writer, write_buf, chunk_size, running};
    read_op rop{reader, read_buf, total_read};

    perf::stopwatch sw;

    wop.start();
    rop.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
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
    auto [sock1, sock2] = asio_bench::make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');
    std::vector<char> rbuf1(chunk_size);
    std::vector<char> rbuf2(chunk_size);

    std::atomic<bool> running{true};
    std::size_t read1 = 0;
    std::size_t read2 = 0;

    // sock1 writes, sock2 reads (direction 1)
    write_op wop1{sock1, buf1, chunk_size, running};
    read_op rop1{sock2, rbuf1, read1};

    // sock2 writes, sock1 reads (direction 2)
    write_op wop2{sock2, buf2, chunk_size, running};
    read_op rop2{sock1, rbuf2, read2};

    perf::stopwatch sw;

    wop1.start();
    rop1.start();
    wop2.start();
    rop2.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(static_cast<int64_t>(read1 + read2));

    sock1.close();
    sock2.close();
}

struct mt_write_op
{
    tcp_socket& sock;
    std::vector<char>& buf;
    std::size_t chunk_size;
    std::atomic<bool>& running;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
        {
            sock.shutdown(tcp_socket::shutdown_send);
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

struct mt_read_op
{
    tcp_socket& sock;
    std::vector<char>& buf;
    std::atomic<std::size_t>& total_read;

    void start()
    {
        sock.async_read_some(
            asio::buffer(buf.data(), buf.size()),
            [this](boost::system::error_code ec, std::size_t n) {
                if (ec || n == 0)
                    return;
                total_read.fetch_add(n, std::memory_order_relaxed);
                start();
            });
    }
};

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
        std::vector<char> rbuf1;
        std::vector<char> rbuf2;
    };

    std::vector<tcp_socket> sock1s;
    std::vector<tcp_socket> sock2s;
    std::vector<pair_bufs> bufs;

    sock1s.reserve(num_connections);
    sock2s.reserve(num_connections);
    bufs.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [s1, s2] = asio_bench::make_socket_pair(ioc);
        sock1s.push_back(std::move(s1));
        sock2s.push_back(std::move(s2));
        bufs.push_back(
            {std::vector<char>(chunk_size, 'a'),
             std::vector<char>(chunk_size, 'b'), std::vector<char>(chunk_size),
             std::vector<char>(chunk_size)});
    }

    std::atomic<bool> running{true};
    std::atomic<std::size_t> total_read{0};

    std::vector<std::unique_ptr<mt_write_op>> write_ops;
    std::vector<std::unique_ptr<mt_read_op>> read_ops;

    for (int i = 0; i < num_connections; ++i)
    {
        // Direction 1: sock1 writes, sock2 reads
        write_ops.push_back(
            std::make_unique<mt_write_op>(
                mt_write_op{sock1s[i], bufs[i].wbuf1, chunk_size, running}));
        read_ops.push_back(
            std::make_unique<mt_read_op>(
                mt_read_op{sock2s[i], bufs[i].rbuf1, total_read}));

        // Direction 2: sock2 writes, sock1 reads
        write_ops.push_back(
            std::make_unique<mt_write_op>(
                mt_write_op{sock2s[i], bufs[i].wbuf2, chunk_size, running}));
        read_ops.push_back(
            std::make_unique<mt_read_op>(
                mt_read_op{sock1s[i], bufs[i].rbuf2, total_read}));
    }

    perf::stopwatch sw;

    for (auto& w : write_ops)
        w->start();
    for (auto& r : read_ops)
        r->start();

    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int i = 1; i < num_threads; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();

    timer.join();
    for (auto& t : threads)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_bytes(
        static_cast<int64_t>(total_read.load(std::memory_order_relaxed)));

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
            auto [w, r] = asio_bench::make_socket_pair(ioc);
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

} // namespace asio_callback_bench
