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
#include <cstring>
#include <iostream>
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

#include "../common/benchmark.hpp"
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
bench::benchmark_result
bench_throughput(std::size_t chunk_size, double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    std::cout << "  Buffer size: " << chunk_size << " bytes\n";

    corosio::native_io_context<Backend> ioc;
    auto [writer, reader] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    set_nodelay(writer);
    set_nodelay(reader);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_written = 0;
    std::size_t total_read    = 0;

    auto write_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), chunk_size));
            if (ec)
                break;
            total_written += n;
        }
        writer.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read_task = [&]() -> capy::task<> {
        for (;;)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec || n == 0)
                break;
            total_read += n;
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(write_task());
    capy::run_async(ioc.get_executor())(read_task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    double elapsed    = sw.elapsed_seconds();
    double throughput = static_cast<double>(total_read) / elapsed;

    std::cout << "    Written:    " << total_written << " bytes\n";
    std::cout << "    Read:       " << total_read << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_throughput(throughput)
              << "\n\n";

    writer.close();
    reader.close();

    return bench::benchmark_result("throughput_" + std::to_string(chunk_size))
        .add("chunk_size", static_cast<double>(chunk_size))
        .add("bytes_written", static_cast<double>(total_written))
        .add("bytes_read", static_cast<double>(total_read))
        .add("elapsed_s", elapsed)
        .add("throughput_bytes_per_sec", throughput);
}

template<auto Backend>
bench::benchmark_result
bench_bidirectional_throughput(std::size_t chunk_size, double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    std::cout << "  Buffer size: " << chunk_size << " bytes, bidirectional\n";

    corosio::native_io_context<Backend> ioc;
    auto [sock1, sock2] = corosio::test::make_socket_pair<
        socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);

    set_nodelay(sock1);
    set_nodelay(sock2);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    auto write1_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), chunk_size));
            if (ec)
                break;
            written1 += n;
        }
        sock1.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read1_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read1 += n;
        }
    };

    auto write2_task = [&]() -> capy::task<> {
        while (running.load(std::memory_order_relaxed))
        {
            auto [ec, n] = co_await sock2.write_some(
                capy::const_buffer(buf2.data(), chunk_size));
            if (ec)
                break;
            written2 += n;
        }
        sock2.shutdown( corosio::tcp_socket::shutdown_send );
    };

    auto read2_task = [&]() -> capy::task<> {
        std::vector<char> rbuf(chunk_size);
        for (;;)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0)
                break;
            read2 += n;
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(write1_task());
    capy::run_async(ioc.get_executor())(read1_task());
    capy::run_async(ioc.get_executor())(write2_task());
    capy::run_async(ioc.get_executor())(read2_task());

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    double elapsed                = sw.elapsed_seconds();
    std::size_t total_transferred = read1 + read2;
    double throughput = static_cast<double>(total_transferred) / elapsed;

    std::cout << "    Direction 1: " << read1 << " bytes\n";
    std::cout << "    Direction 2: " << read2 << " bytes\n";
    std::cout << "    Total:       " << total_transferred << " bytes\n";
    std::cout << "    Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput:  " << perf::format_throughput(throughput)
              << " (combined)\n\n";

    sock1.close();
    sock2.close();

    return bench::benchmark_result(
               "bidirectional_" + std::to_string(chunk_size))
        .add("chunk_size", static_cast<double>(chunk_size))
        .add("bytes_direction1", static_cast<double>(read1))
        .add("bytes_direction2", static_cast<double>(read2))
        .add("total_transferred", static_cast<double>(total_transferred))
        .add("elapsed_s", elapsed)
        .add("throughput_bytes_per_sec", throughput);
}

// Free coroutine functions avoid dangling-this when spawned in a loop
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
    std::atomic<std::size_t>& total_read)
{
    std::vector<char> rbuf(chunk_size);
    for (;;)
    {
        auto [ec, n] = co_await sock.read_some(
            capy::mutable_buffer(rbuf.data(), rbuf.size()));
        if (ec || n == 0)
            break;
        total_read.fetch_add(n, std::memory_order_relaxed);
    }
}

template<auto Backend>
bench::benchmark_result
bench_multithread_throughput(
    int num_threads,
    int num_connections,
    std::size_t chunk_size,
    double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    std::cout << "  Threads: " << num_threads
              << ", Connections: " << num_connections
              << ", Buffer: " << chunk_size << " bytes\n";

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
    std::atomic<std::size_t> total_read{0};

    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(mt_write_coro<Backend>(
            sock1s[i], bufs[i].wbuf1, chunk_size, running));
        capy::run_async(ioc.get_executor())(
            mt_read_coro<Backend>(sock2s[i], chunk_size, total_read));
        capy::run_async(ioc.get_executor())(mt_write_coro<Backend>(
            sock2s[i], bufs[i].wbuf2, chunk_size, running));
        capy::run_async(ioc.get_executor())(
            mt_read_coro<Backend>(sock1s[i], chunk_size, total_read));
    }

    perf::stopwatch sw;

    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int i = 1; i < num_threads; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();

    timer.join();
    for (auto& t : threads)
        t.join();

    double elapsed    = sw.elapsed_seconds();
    std::size_t bytes = total_read.load(std::memory_order_relaxed);
    double throughput = static_cast<double>(bytes) / elapsed;

    std::cout << "    Total read: " << bytes << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_throughput(throughput)
              << " (combined)\n\n";

    for (auto& s : sock1s)
        s.close();
    for (auto& s : sock2s)
        s.close();

    return bench::benchmark_result(
               "multithread_" + std::to_string(num_threads) + "t_" +
               std::to_string(chunk_size))
        .add("num_threads", static_cast<double>(num_threads))
        .add("num_connections", static_cast<double>(num_connections))
        .add("chunk_size", static_cast<double>(chunk_size))
        .add("total_read", static_cast<double>(bytes))
        .add("elapsed_s", elapsed)
        .add("throughput_bytes_per_sec", throughput);
}

} // anonymous namespace

template<auto Backend>
void
run_socket_throughput_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s)
{
    using socket_type = corosio::native_tcp_socket<Backend>;

    (void)factory;

    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    // Warm up
    {
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
    }

    std::vector<std::size_t> buffer_sizes = {1024,   4096,   16384,  65536,
                                             131072, 262144, 524288, 1048576};

    if (run_all || std::strcmp(filter, "unidirectional") == 0)
    {
        perf::print_header("Unidirectional Throughput (Corosio)");
        for (auto size : buffer_sizes)
            collector.add(bench_throughput<Backend>(size, duration_s));
    }

    if (run_all || std::strcmp(filter, "bidirectional") == 0)
    {
        perf::print_header("Bidirectional Throughput (Corosio)");
        for (auto size : buffer_sizes)
            collector.add(
                bench_bidirectional_throughput<Backend>(size, duration_s));
    }

    if (run_all || std::strcmp(filter, "multithread") == 0)
    {
        int thread_counts[]    = {2, 4, 8};
        std::size_t mt_sizes[] = {65536, 131072, 262144, 524288};
        for (auto tc : thread_counts)
        {
            std::string hdr = "Multithread Throughput " + std::to_string(tc) +
                " threads (Corosio)";
            perf::print_header(hdr.c_str());
            for (auto size : mt_sizes)
                collector.add(
                    bench_multithread_throughput<Backend>(
                        tc, 32, size, duration_s));
        }
    }
}

} // namespace corosio_bench

COROSIO_BENCH_INSTANTIATE(void corosio_bench::run_socket_throughput_benchmarks)
