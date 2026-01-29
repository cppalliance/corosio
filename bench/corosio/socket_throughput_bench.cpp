//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <iostream>
#include <vector>

#if BOOST_COROSIO_HAS_IOCP
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "../common/backend_selection.hpp"
#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Helper to set TCP_NODELAY on a socket for low latency
inline void set_nodelay(corosio::socket& s)
{
    int flag = 1;
#if BOOST_COROSIO_HAS_IOCP
    ::setsockopt(static_cast<SOCKET>(s.native_handle()), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    ::setsockopt(s.native_handle(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif
}

// Measures maximum unidirectional data transfer rate over a loopback socket pair.
// One coroutine writes while another reads, testing the efficiency of async I/O
// operations. Runs with different buffer sizes to reveal the optimal chunk size
// for this platform. Small buffers stress syscall overhead; large buffers approach
// memory bandwidth limits. Useful for tuning buffer sizes in streaming protocols.
template<typename Context>
bench::benchmark_result bench_throughput(std::size_t chunk_size, std::size_t total_bytes)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << (total_bytes / (1024 * 1024)) << " MB\n";

    Context ioc;
    auto [writer, reader] = corosio::test::make_socket_pair(ioc);

    // Disable Nagle's algorithm for fair comparison with Asio
    set_nodelay(writer);
    set_nodelay(reader);

    std::vector<char> write_buf(chunk_size, 'x');
    std::vector<char> read_buf(chunk_size);

    std::size_t total_written = 0;
    std::size_t total_read = 0;
    bool writer_done = false;

    // Writer coroutine
    auto write_task = [&]() -> capy::task<>
    {
        while (total_written < total_bytes)
        {
            std::size_t to_write = (std::min)(chunk_size, total_bytes - total_written);
            auto [ec, n] = co_await writer.write_some(
                capy::const_buffer(write_buf.data(), to_write));
            if (ec)
            {
                std::cerr << "    Write error: " << ec.message() << "\n";
                break;
            }
            total_written += n;
        }
        writer_done = true;
        writer.shutdown(corosio::socket::shutdown_send);
    };

    // Reader coroutine
    auto read_task = [&]() -> capy::task<>
    {
        while (total_read < total_bytes)
        {
            auto [ec, n] = co_await reader.read_some(
                capy::mutable_buffer(read_buf.data(), read_buf.size()));
            if (ec)
            {
                if (writer_done && total_read >= total_bytes)
                    break;
                std::cerr << "    Read error: " << ec.message() << "\n";
                break;
            }
            if (n == 0)
                break;
            total_read += n;
        }
    };

    bench::stopwatch sw;

    capy::run_async(ioc.get_executor())(write_task());
    capy::run_async(ioc.get_executor())(read_task());
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    double throughput = static_cast<double>(total_read) / elapsed;

    std::cout << "    Written:    " << total_written << " bytes\n";
    std::cout << "    Read:       " << total_read << " bytes\n";
    std::cout << "    Elapsed:    " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_throughput(throughput) << "\n\n";

    writer.close();
    reader.close();

    return bench::benchmark_result("throughput_" + std::to_string(chunk_size))
        .add("chunk_size", static_cast<double>(chunk_size))
        .add("total_bytes", static_cast<double>(total_bytes))
        .add("bytes_written", static_cast<double>(total_written))
        .add("bytes_read", static_cast<double>(total_read))
        .add("elapsed_s", elapsed)
        .add("throughput_bytes_per_sec", throughput);
}

// Measures full-duplex throughput with both endpoints sending and receiving
// simultaneously. Four concurrent coroutines (two writers, two readers) stress
// the scheduler's ability to multiplex I/O efficiently. This pattern is common
// in protocols like WebSocket or gRPC where data flows in both directions.
// Combined throughput should ideally approach 2x unidirectional throughput.
template<typename Context>
bench::benchmark_result bench_bidirectional_throughput(std::size_t chunk_size, std::size_t total_bytes)
{
    std::cout << "  Buffer size: " << chunk_size << " bytes, ";
    std::cout << "Transfer: " << (total_bytes / (1024 * 1024)) << " MB each direction\n";

    Context ioc;
    auto [sock1, sock2] = corosio::test::make_socket_pair(ioc);

    // Disable Nagle's algorithm for fair comparison with Asio
    set_nodelay(sock1);
    set_nodelay(sock2);

    std::vector<char> buf1(chunk_size, 'a');
    std::vector<char> buf2(chunk_size, 'b');

    std::size_t written1 = 0, read1 = 0;
    std::size_t written2 = 0, read2 = 0;

    // Socket 1 writes to socket 2
    auto write1_task = [&]() -> capy::task<>
    {
        while (written1 < total_bytes)
        {
            std::size_t to_write = (std::min)(chunk_size, total_bytes - written1);
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(buf1.data(), to_write));
            if (ec) break;
            written1 += n;
        }
        sock1.shutdown(corosio::socket::shutdown_send);
    };

    // Socket 2 reads from socket 1
    auto read1_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf(chunk_size);
        while (read1 < total_bytes)
        {
            auto [ec, n] = co_await sock2.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0) break;
            read1 += n;
        }
    };

    // Socket 2 writes to socket 1
    auto write2_task = [&]() -> capy::task<>
    {
        while (written2 < total_bytes)
        {
            std::size_t to_write = (std::min)(chunk_size, total_bytes - written2);
            auto [ec, n] = co_await sock2.write_some(
                capy::const_buffer(buf2.data(), to_write));
            if (ec) break;
            written2 += n;
        }
        sock2.shutdown(corosio::socket::shutdown_send);
    };

    // Socket 1 reads from socket 2
    auto read2_task = [&]() -> capy::task<>
    {
        std::vector<char> rbuf(chunk_size);
        while (read2 < total_bytes)
        {
            auto [ec, n] = co_await sock1.read_some(
                capy::mutable_buffer(rbuf.data(), rbuf.size()));
            if (ec || n == 0) break;
            read2 += n;
        }
    };

    bench::stopwatch sw;

    capy::run_async(ioc.get_executor())(write1_task());
    capy::run_async(ioc.get_executor())(read1_task());
    capy::run_async(ioc.get_executor())(write2_task());
    capy::run_async(ioc.get_executor())(read2_task());
    ioc.run();

    double elapsed = sw.elapsed_seconds();
    std::size_t total_transferred = read1 + read2;
    double throughput = static_cast<double>(total_transferred) / elapsed;

    std::cout << "    Direction 1: " << read1 << " bytes\n";
    std::cout << "    Direction 2: " << read2 << " bytes\n";
    std::cout << "    Total:       " << total_transferred << " bytes\n";
    std::cout << "    Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput:  " << bench::format_throughput(throughput)
              << " (combined)\n\n";

    sock1.close();
    sock2.close();

    return bench::benchmark_result("bidirectional_" + std::to_string(chunk_size))
        .add("chunk_size", static_cast<double>(chunk_size))
        .add("total_bytes_per_direction", static_cast<double>(total_bytes))
        .add("bytes_direction1", static_cast<double>(read1))
        .add("bytes_direction2", static_cast<double>(read2))
        .add("total_transferred", static_cast<double>(total_transferred))
        .add("elapsed_s", elapsed)
        .add("throughput_bytes_per_sec", throughput);
}

// Run benchmarks for a specific context type
template<typename Context>
void run_benchmarks(const char* backend_name, const char* output_file, const char* bench_filter)
{
    std::cout << "Boost.Corosio Socket Throughput Benchmarks\n";
    std::cout << "==========================================\n";
    std::cout << "Backend: " << backend_name << "\n\n";

    bench::result_collector collector(backend_name);

    bool run_all = !bench_filter || std::strcmp(bench_filter, "all") == 0;

    // Variable buffer sizes
    std::vector<std::size_t> buffer_sizes = {1024, 4096, 16384, 65536};
    std::size_t transfer_size = 64 * 1024 * 1024; // 64 MB

    if (run_all || std::strcmp(bench_filter, "unidirectional") == 0)
    {
        bench::print_header("Unidirectional Throughput");
        for (auto size : buffer_sizes)
            collector.add(bench_throughput<Context>(size, transfer_size));
    }

    if (run_all || std::strcmp(bench_filter, "bidirectional") == 0)
    {
        bench::print_header("Bidirectional Throughput");
        for (auto size : buffer_sizes)
            collector.add(bench_bidirectional_throughput<Context>(size, transfer_size / 2));
    }

    std::cout << "\nBenchmarks complete.\n";

    if (output_file)
    {
        if (collector.write_json(output_file))
            std::cout << "Results written to: " << output_file << "\n";
        else
            std::cerr << "Error: Failed to write results to: " << output_file << "\n";
    }
}

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --backend <name>   Select I/O backend (default: platform default)\n";
    std::cout << "  --bench <name>     Run only the specified benchmark\n";
    std::cout << "  --output <file>    Write JSON results to file\n";
    std::cout << "  --list             List available backends\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    std::cout << "Available benchmarks:\n";
    std::cout << "  unidirectional     Unidirectional throughput (various buffer sizes)\n";
    std::cout << "  bidirectional      Bidirectional throughput (various buffer sizes)\n";
    std::cout << "  all                Run all benchmarks (default)\n";
    std::cout << "\n";
    bench::print_available_backends();
}

int main(int argc, char* argv[])
{
    const char* backend = nullptr;
    const char* output_file = nullptr;
    const char* bench_filter = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--backend") == 0)
        {
            if (i + 1 < argc)
            {
                backend = argv[++i];
            }
            else
            {
                std::cerr << "Error: --backend requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--bench") == 0)
        {
            if (i + 1 < argc)
            {
                bench_filter = argv[++i];
            }
            else
            {
                std::cerr << "Error: --bench requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--output") == 0)
        {
            if (i + 1 < argc)
            {
                output_file = argv[++i];
            }
            else
            {
                std::cerr << "Error: --output requires an argument\n";
                return 1;
            }
        }
        else if (std::strcmp(argv[i], "--list") == 0)
        {
            bench::print_available_backends();
            return 0;
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // If no backend specified, use platform default
    if (!backend)
        backend = bench::default_backend_name();

    // Dispatch to the selected backend using a generic lambda
    return bench::dispatch_backend(backend,
        [=]<typename Context>(const char* name)
        {
            run_benchmarks<Context>(name, output_file, bench_filter);
        });
}
