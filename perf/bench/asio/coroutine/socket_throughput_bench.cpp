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

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio_bench {
namespace {

// Pattern C: Write until running=false, then shutdown; reader reads until EOF
bench::benchmark_result
bench_throughput(std::size_t chunk_size, double duration_s)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes\n";

    asio::io_context ioc;
    auto [writer, reader] = make_socket_pair(ioc);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::atomic<bool> running{true};
    std::size_t total_written = 0;
    std::size_t total_read    = 0;

    auto write_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto n = co_await writer.async_write_some(
                    asio::buffer(write_buf.data(), chunk_size), asio::deferred);
                total_written += n;
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
                total_read += n;
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

bench::benchmark_result
bench_bidirectional_throughput(std::size_t chunk_size, double duration_s)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, bidirectional\n";

    asio::io_context ioc;
    auto [sock1, sock2] = make_socket_pair(ioc);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::atomic<bool> running{true};
    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    auto write1_task = [&]() -> asio::awaitable<void, executor_type> {
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                auto n = co_await sock1.async_write_some(
                    asio::buffer(buf1.data(), chunk_size), asio::deferred);
                written1 += n;
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
                read1 += n;
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
                auto n = co_await sock2.async_write_some(
                    asio::buffer(buf2.data(), chunk_size), asio::deferred);
                written2 += n;
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
                read2 += n;
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
    std::atomic<std::size_t>& total_read)
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
            total_read.fetch_add(n, std::memory_order_relaxed);
        }
    }
    catch (std::exception const&)
    {
    }
}

bench::benchmark_result
bench_multithread_throughput(
    int num_threads,
    int num_connections,
    std::size_t chunk_size,
    double duration_s)
{
    std::cout << "  Threads: " << num_threads
              << ", Connections: " << num_connections
              << ", Buffer: " << chunk_size << " bytes\n";

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
    std::atomic<std::size_t> total_read{0};

    for (int i = 0; i < num_connections; ++i)
    {
        asio::co_spawn(
            ioc, mt_write_coro(sock1s[i], bufs[i].wbuf1, chunk_size, running),
            asio::detached);
        asio::co_spawn(
            ioc, mt_read_coro(sock2s[i], chunk_size, total_read),
            asio::detached);
        asio::co_spawn(
            ioc, mt_write_coro(sock2s[i], bufs[i].wbuf2, chunk_size, running),
            asio::detached);
        asio::co_spawn(
            ioc, mt_read_coro(sock1s[i], chunk_size, total_read),
            asio::detached);
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

void
run_socket_throughput_benchmarks(
    bench::result_collector& collector, char const* filter, double duration_s)
{
    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    // Warm up
    {
        asio::io_context ioc;
        auto [w, r] = make_socket_pair(ioc);
        std::vector<char> buf(4096, 'w');
        asio::write(w, asio::buffer(buf));
        asio::read(r, asio::buffer(buf));
        w.close();
        r.close();
    }

    std::vector<std::size_t> buffer_sizes = {1024,   4096,   16384,  65536,
                                             131072, 262144, 524288, 1048576};

    if (run_all || std::strcmp(filter, "unidirectional") == 0)
    {
        perf::print_header("Unidirectional Throughput (Asio Coroutines)");
        for (auto size : buffer_sizes)
            collector.add(bench_throughput(size, duration_s));
    }

    if (run_all || std::strcmp(filter, "bidirectional") == 0)
    {
        perf::print_header("Bidirectional Throughput (Asio Coroutines)");
        for (auto size : buffer_sizes)
            collector.add(bench_bidirectional_throughput(size, duration_s));
    }

    if (run_all || std::strcmp(filter, "multithread") == 0)
    {
        int thread_counts[]    = {2, 4, 8};
        std::size_t mt_sizes[] = {65536, 131072, 262144, 524288};
        for (auto tc : thread_counts)
        {
            std::string hdr = "Multithread Throughput " + std::to_string(tc) +
                " threads (Asio Coroutines)";
            perf::print_header(hdr.c_str());
            for (auto size : mt_sizes)
                collector.add(
                    bench_multithread_throughput(tc, 32, size, duration_s));
        }
    }
}

} // namespace asio_bench
