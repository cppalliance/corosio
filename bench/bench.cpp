//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "instrumentation.hpp"
#include "callback/handler.hpp"
#include "callback/operations.hpp"
#include "callback/socket.hpp"
#include "callback/tls_stream.hpp"
#include "coroutine/operations.hpp"

#include <capy/async_run.hpp>
#include <corosio/io_context.hpp>
#include <corosio/socket.hpp>
#include <corosio/tls_stream.hpp>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>

struct bench_result
{
    long long ns;
    std::size_t allocs;
    std::size_t ios;
    std::size_t works;
};

struct bench_test
{
    static constexpr int N = 100000;

    template<class Socket, class AsyncOp>
    static bench_result bench(Socket& sock, AsyncOp op)
    {
        using clock = std::chrono::high_resolution_clock;
        auto& ioc = *sock.get_executor().ctx_;
        int count = 0;

        g_alloc_count = 0;
        g_io_count = 0;
        g_work_count = 0;
        auto t0 = clock::now();
        for(int i = 0; i < N; ++i)
        {
            callback::handler cb(count);
            op(sock, cb);
            ioc.run();
        }
        auto t1 = clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return {ns / N, g_alloc_count / N, g_io_count / N, g_work_count / N};
    }

    template<class MakeTask>
    static bench_result bench_co(corosio::io_context& ioc, MakeTask make_task)
    {
        using clock = std::chrono::high_resolution_clock;
        int count = 0;

        g_alloc_count = 0;
        g_io_count = 0;
        g_work_count = 0;
        auto t0 = clock::now();
        for(int i = 0; i < N; ++i)
        {
            capy::async_run(ioc.get_executor(), make_task(count));
            ioc.run();
        }
        auto t1 = clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return {ns / N, g_alloc_count / N, g_io_count / N, g_work_count / N};
    }

    static void print_line(int level, char const* stream_type, char const* op_name,
        char const* style, bench_result const& r, bench_result const& other)
    {
        std::cout << level << " " << std::left << std::setw(11) << stream_type << std::setw(11)
                  << op_name << std::setw(3) << style << ": " << std::right << std::setw(5) << r.ns
                  << " ns/op";
        if(r.allocs != 0)
            std::cout << ", " << r.allocs << " allocs/op";
        if(r.ios != other.ios)
            std::cout << ", " << r.ios << " io/op";
        if(r.works != other.works)
            std::cout << ", " << r.works << " work/op";
        std::cout << "\n";
    }

    static void print_results(int level, char const* stream_type, char const* op_name,
        bench_result const& cb, bench_result const& co)
    {
        print_line(level, stream_type, op_name, "cb", cb, co);
        print_line(level, stream_type, op_name, "co", co, cb);
    }

    void run()
    {
        corosio::io_context ioc;
        callback::socket cb_sock(ioc);
        corosio::socket co_sock(ioc);
        callback::tls_stream<callback::socket> cb_tls(ioc);
        corosio::tls_stream<corosio::socket> co_tls(ioc);

        bench_result cb, co;

        // socket read_some (1 call) - level 1
        cb = bench(cb_sock, [](auto& sock, auto h) { sock.async_read_some(std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await co_sock.async_read_some();
            ++count;
        });
        print_results(1, "socket", "read_some", cb, co);

        // tls_stream read_some (1 call) - level 1
        cb = bench(cb_tls, [](auto& sock, auto h) { sock.async_read_some(std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await co_tls.async_read_some();
            ++count;
        });
        print_results(1, "tls_stream", "read_some", cb, co);

        std::cout << "\n";

        // socket read (5 calls) - level 2
        cb = bench(cb_sock, [](auto& sock, auto h) { callback::async_read(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_read(co_sock);
            ++count;
        });
        print_results(2, "socket", "read", cb, co);

        // tls_stream read (5 calls) - level 2
        cb = bench(cb_tls, [](auto& sock, auto h) { callback::async_read(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_read(co_tls);
            ++count;
        });
        print_results(2, "tls_stream", "read", cb, co);

        std::cout << "\n";

        // socket request (10 calls) - level 2
        cb =
            bench(cb_sock, [](auto& sock, auto h) { callback::async_request(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_request(co_sock);
            ++count;
        });
        print_results(3, "socket", "request", cb, co);

        // tls_stream request (10 calls) - level 2
        cb = bench(cb_tls, [](auto& sock, auto h) { callback::async_request(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_request(co_tls);
            ++count;
        });
        print_results(3, "tls_stream", "request", cb, co);

        std::cout << "\n";

        // socket session (1000 calls) - level 3
        cb =
            bench(cb_sock, [](auto& sock, auto h) { callback::async_session(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_session(co_sock);
            ++count;
        });
        print_results(4, "socket", "session", cb, co);

        // tls_stream session (1000 calls) - level 3
        cb = bench(cb_tls, [](auto& sock, auto h) { callback::async_session(sock, std::move(h)); });
        co = bench_co(ioc, [&](int& count) -> capy::task {
            co_await coroutine::async_session(co_tls);
            ++count;
        });
        print_results(4, "tls_stream", "session", cb, co);
    }
};

int main()
{
    bench_test t;
    t.run();
    return 0;
}

