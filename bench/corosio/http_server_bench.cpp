//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/epoll_context.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/read_until.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"
#include "../common/http_protocol.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Server coroutine: reads requests and sends responses
capy::task<> server_task(
    corosio::socket& sock,
    int num_requests,
    int& completed_requests)
{
    std::string buf;

    while (completed_requests < num_requests)
    {
        // Read until end of HTTP headers
        auto [ec, n] = co_await capy::read_until(
            sock, capy::dynamic_buffer(buf), "\r\n\r\n");
        if (ec)
            co_return;

        // Send response
        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(bench::http::small_response, bench::http::small_response_size));
        if (wec)
            co_return;

        ++completed_requests;
        buf.erase(0, n);
    }
}

// Client coroutine: sends requests and reads responses
capy::task<> client_task(
    corosio::socket& sock,
    int num_requests,
    bench::statistics& latency_stats)
{
    std::string buf;

    for (int i = 0; i < num_requests; ++i)
    {
        bench::stopwatch sw;

        // Send request
        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(bench::http::small_request, bench::http::small_request_size));
        if (wec)
            co_return;

        // Read response headers
        auto [ec, header_end] = co_await capy::read_until(
            sock, capy::dynamic_buffer(buf), "\r\n\r\n");
        if (ec)
            co_return;

        // Parse Content-Length from headers and read body if needed
        std::string_view headers(buf.data(), header_end);
        std::size_t content_length = 0;
        auto pos = headers.find("Content-Length: ");
        if (pos != std::string_view::npos)
        {
            pos += 16;
            while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9')
            {
                content_length = content_length * 10 + (headers[pos] - '0');
                ++pos;
            }
        }

        // Read body if not already in buffer
        std::size_t total_size = header_end + content_length;
        if (buf.size() < total_size)
        {
            std::size_t need = total_size - buf.size();
            std::size_t old_size = buf.size();
            buf.resize(total_size);
            auto [rec, rn] = co_await capy::read(
                sock, capy::mutable_buffer(buf.data() + old_size, need));
            if (rec)
                co_return;
        }

        double latency_us = sw.elapsed_us();
        latency_stats.add(latency_us);

        buf.erase(0, total_size);
    }
}

// Single connection benchmark
bench::benchmark_result bench_single_connection(int num_requests)
{
    std::cout << "  Requests: " << num_requests << "\n";

    corosio::io_context ioc;
    auto [client, server] = corosio::test::make_socket_pair(ioc);

    client.set_no_delay(true);
    server.set_no_delay(true);

    int completed_requests = 0;
    bench::statistics latency_stats;

    bench::stopwatch total_sw;

    capy::run_async(ioc.get_executor())(
        server_task(server, num_requests, completed_requests));
    capy::run_async(ioc.get_executor())(
        client_task(client, num_requests, latency_stats));

    ioc.run();

    double elapsed = total_sw.elapsed_seconds();
    double requests_per_sec = static_cast<double>(num_requests) / elapsed;

    std::cout << "    Completed: " << num_requests << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_rate(requests_per_sec) << "\n";
    bench::print_latency_stats(latency_stats, "Request latency");
    std::cout << "\n";

    client.close();
    server.close();

    return bench::benchmark_result("single_conn")
        .add("num_requests", num_requests)
        .add("num_connections", 1)
        .add("requests_per_sec", requests_per_sec)
        .add_latency_stats("request_latency", latency_stats);
}

// Concurrent connections benchmark
bench::benchmark_result bench_concurrent_connections(int num_connections, int requests_per_conn)
{
    int total_requests = num_connections * requests_per_conn;
    std::cout << "  Connections: " << num_connections
              << ", Requests per connection: " << requests_per_conn
              << ", Total: " << total_requests << "\n";

    corosio::io_context ioc;

    std::vector<corosio::socket> clients;
    std::vector<corosio::socket> servers;
    std::vector<int> completed(num_connections, 0);
    std::vector<bench::statistics> stats(num_connections);

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair(ioc);
        c.set_no_delay(true);
        s.set_no_delay(true);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    bench::stopwatch total_sw;

    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(
            server_task(servers[i], requests_per_conn, completed[i]));
        capy::run_async(ioc.get_executor())(
            client_task(clients[i], requests_per_conn, stats[i]));
    }

    ioc.run();

    double elapsed = total_sw.elapsed_seconds();
    double requests_per_sec = static_cast<double>(total_requests) / elapsed;

    // Aggregate latency stats
    double total_mean = 0;
    double total_p99 = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Completed: " << total_requests << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_rate(requests_per_sec) << "\n";
    std::cout << "    Avg mean latency: "
              << bench::format_latency(total_mean / num_connections) << "\n";
    std::cout << "    Avg p99 latency: "
              << bench::format_latency(total_p99 / num_connections) << "\n\n";

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();

    return bench::benchmark_result("concurrent_" + std::to_string(num_connections))
        .add("num_connections", num_connections)
        .add("requests_per_conn", requests_per_conn)
        .add("total_requests", total_requests)
        .add("requests_per_sec", requests_per_sec)
        .add("avg_mean_latency_us", total_mean / num_connections)
        .add("avg_p99_latency_us", total_p99 / num_connections);
}

// Multi-threaded benchmark: multiple threads calling run()
bench::benchmark_result bench_multithread(int num_threads, int num_connections, int requests_per_conn)
{
    int total_requests = num_connections * requests_per_conn;
    std::cout << "  Threads: " << num_threads
              << ", Connections: " << num_connections
              << ", Requests per connection: " << requests_per_conn
              << ", Total: " << total_requests << "\n";

    // Use default io_context for socket creation (make_socket_pair calls run() internally)
    corosio::io_context ioc;

    std::vector<corosio::socket> clients;
    std::vector<corosio::socket> servers;
    std::vector<int> completed(num_connections, 0);
    std::vector<bench::statistics> stats(num_connections);

    clients.reserve(num_connections);
    servers.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair(ioc);
        c.set_no_delay(true);
        s.set_no_delay(true);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    // Spawn all coroutines before starting threads
    for (int i = 0; i < num_connections; ++i)
    {
        capy::run_async(ioc.get_executor())(
            server_task(servers[i], requests_per_conn, completed[i]));
        capy::run_async(ioc.get_executor())(
            client_task(clients[i], requests_per_conn, stats[i]));
    }

    bench::stopwatch total_sw;

    // Launch worker threads
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (int i = 1; i < num_threads; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    // Main thread also runs
    ioc.run();

    // Wait for all threads
    for (auto& t : threads)
        t.join();

    double elapsed = total_sw.elapsed_seconds();
    double requests_per_sec = static_cast<double>(total_requests) / elapsed;

    // Aggregate latency stats
    double total_mean = 0;
    double total_p99 = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Completed: " << total_requests << " requests\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << bench::format_rate(requests_per_sec) << "\n";
    std::cout << "    Avg mean latency: "
              << bench::format_latency(total_mean / num_connections) << "\n";
    std::cout << "    Avg p99 latency: "
              << bench::format_latency(total_p99 / num_connections) << "\n\n";

    for (auto& c : clients)
        c.close();
    for (auto& s : servers)
        s.close();

    return bench::benchmark_result("multithread_" + std::to_string(num_threads) + "t")
        .add("num_threads", num_threads)
        .add("num_connections", num_connections)
        .add("requests_per_conn", requests_per_conn)
        .add("total_requests", total_requests)
        .add("requests_per_sec", requests_per_sec)
        .add("avg_mean_latency_us", total_mean / num_connections)
        .add("avg_p99_latency_us", total_p99 / num_connections);
}

void run_benchmarks(char const* output_file, char const* bench_filter)
{
    std::cout << "Boost.Corosio HTTP Server Benchmarks\n";
    std::cout << "====================================\n";

    bench::result_collector collector("corosio");

    bool run_all = !bench_filter || std::strcmp(bench_filter, "all") == 0;

    if (run_all || std::strcmp(bench_filter, "single_conn") == 0)
    {
        bench::print_header("Single Connection (Sequential Requests)");
        collector.add(bench_single_connection(10000));
    }

    if (run_all || std::strcmp(bench_filter, "concurrent") == 0)
    {
        if (run_all)
            std::this_thread::sleep_for(std::chrono::seconds(5));
        bench::print_header("Concurrent Connections");
        collector.add(bench_concurrent_connections(1, 10000));
        collector.add(bench_concurrent_connections(4, 2500));
        collector.add(bench_concurrent_connections(16, 625));
        collector.add(bench_concurrent_connections(32, 312));
    }

    if (run_all || std::strcmp(bench_filter, "multithread") == 0)
    {
        if (run_all)
            std::this_thread::sleep_for(std::chrono::seconds(5));
        bench::print_header("Multi-threaded (32 connections, varying threads)");
        collector.add(bench_multithread(1, 32, 312));
        collector.add(bench_multithread(2, 32, 312));
        collector.add(bench_multithread(4, 32, 312));
        collector.add(bench_multithread(8, 32, 312));
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

void print_usage(char const* program_name)
{
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --bench <name>     Run only the specified benchmark\n";
    std::cout << "  --output <file>    Write JSON results to file\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\n";
    std::cout << "Available benchmarks:\n";
    std::cout << "  single_conn        Single connection, sequential requests\n";
    std::cout << "  concurrent         Multiple concurrent connections\n";
    std::cout << "  multithread        Multi-threaded with varying thread counts\n";
    std::cout << "  all                Run all benchmarks (default)\n";
}

int main(int argc, char* argv[])
{
    char const* output_file = nullptr;
    char const* bench_filter = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--bench") == 0)
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

    run_benchmarks(output_file, bench_filter);
    return 0;
}
