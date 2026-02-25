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
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_acceptor;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

// Configures a socket for churn benchmarks: minimal kernel buffers
// (this benchmark only exchanges 1 byte) and immediate RST on close
// to avoid TIME_WAIT accumulation. Reducing SO_SNDBUF/SO_RCVBUF from
// the macOS default of 128 KB each prevents ENOBUFS during rapid
// socket creation in concurrent/burst workloads.
static void configure_churn_socket( tcp_socket& s )
{
    s.set_option( asio::socket_base::send_buffer_size( 1024 ) );
    s.set_option( asio::socket_base::receive_buffer_size( 1024 ) );
    s.set_option( asio::socket_base::linger( true, 0 ) );
}

// Creates a listening acceptor with retry. Under rapid socket churn the
// kernel may temporarily lack buffer space (ENOBUFS); a short back-off
// lets resources drain from the previous benchmark run.
static tcp_acceptor make_churn_acceptor( asio::io_context& ioc )
{
    boost::system::error_code ec;
    for( int attempt = 0; attempt < 20; ++attempt )
    {
        if( attempt > 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        tcp_acceptor acc( ioc.get_executor() );
        ec = acc.open( tcp::v4(), ec );
        if( !ec )
            ec = acc.set_option( tcp_acceptor::reuse_address( true ), ec );
        if( !ec )
            ec = acc.bind( tcp::endpoint( tcp::v4(), 0 ), ec );
        if( !ec )
            ec = acc.listen( asio::socket_base::max_listen_connections, ec );
        if( !ec )
            return acc;
    }
    throw boost::system::system_error( ec );
}

// Connect+accept+exchange 1 byte+close, repeat
struct sequential_churn_op
{
    asio::io_context& ioc;
    tcp_acceptor& acc;
    tcp::endpoint ep;
    std::atomic<bool>& running;
    perf::statistics& latency_stats;
    std::atomic<int64_t>& ops;
    std::unique_ptr<tcp_socket> client;
    std::unique_ptr<tcp_socket> server;
    perf::stopwatch sw;
    char byte         = 'X';
    char recv_byte    = 0;
    bool connect_done = false;
    bool accept_done  = false;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;

        sw.reset();
        connect_done = false;
        accept_done = false;
        client = std::make_unique<tcp_socket>( ioc.get_executor() );
        server = std::make_unique<tcp_socket>( ioc.get_executor() );

        boost::system::error_code ec;
        ec = client->open( tcp::v4(), ec );
        if( ec )
        {
            asio::post( ioc, [this]() { start(); } );
            return;
        }
        configure_churn_socket( *client );

        client->async_connect(ep, [this](boost::system::error_code ec) {
            if (ec)
                return;
            connect_done = true;
            if (accept_done)
                do_write();
        });

        acc.async_accept(*server, [this](boost::system::error_code ec) {
            if (ec)
                return;
            accept_done = true;
            if (connect_done)
                do_write();
        });
    }

    void do_write()
    {
        byte = 'X';
        asio::async_write(
            *client, asio::buffer(&byte, 1),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                do_read();
            });
    }

    void do_read()
    {
        recv_byte = 0;
        asio::async_read(
            *server, asio::buffer(&recv_byte, 1),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                finish();
            });
    }

    void finish()
    {
        client->close();
        server->close();

        latency_stats.add(sw.elapsed_ns());
        ops.fetch_add(1, std::memory_order_relaxed);
        start();
    }
};

// Single connect/accept/1-byte-exchange/close loop. Compared against the
// coroutine variant, the difference isolates coroutine suspend/resume overhead.
void
bench_sequential_churn(bench::state& state)
{
    asio::io_context ioc;
    auto acc = make_churn_acceptor( ioc );
    auto ep = tcp::endpoint( asio::ip::address_v4::loopback(), acc.local_endpoint().port() );

    std::atomic<bool> running{true};

    sequential_churn_op op{ioc, acc, ep, running, state.latency(),
                           state.ops(), {}, {}, {}};

    perf::stopwatch total_sw;

    op.start();

    std::thread timer([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    timer.join();

    state.set_elapsed(total_sw.elapsed_seconds());
    acc.close();
}

// N independent accept loops on separate listeners. Reveals whether
// fd allocation or acceptor state scales linearly under callbacks.
void
bench_concurrent_churn(bench::state& state)
{
    int num_loops = static_cast<int>(state.range(0));
    state.counters["num_loops"] = num_loops;

    asio::io_context ioc;
    std::atomic<bool> running{true};

    std::vector<tcp_acceptor> acceptors;
    acceptors.reserve( num_loops );
    for( int i = 0; i < num_loops; ++i )
        acceptors.push_back( make_churn_acceptor( ioc ) );

    std::vector<std::unique_ptr<sequential_churn_op>> ops;
    ops.reserve(num_loops);

    perf::stopwatch total_sw;

    for (int i = 0; i < num_loops; ++i)
    {
        auto ep = tcp::endpoint(
            asio::ip::address_v4::loopback(), acceptors[i].local_endpoint().port() );
        ops.push_back( std::make_unique<sequential_churn_op>(
            sequential_churn_op{ ioc, acceptors[i], ep, running,
                                 state.latency(), state.ops(), {}, {}, {} } ) );
        ops.back()->start();
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(total_sw.elapsed_seconds());
    for( auto& a : acceptors )
        a.close();
}

// Burst: open N connections, accept all, close all, repeat
struct burst_churn_op
{
    asio::io_context& ioc;
    tcp_acceptor& acc;
    tcp::endpoint ep;
    std::atomic<bool>& running;
    perf::statistics& burst_stats;
    std::atomic<int64_t>& ops;
    int burst_size;

    std::vector<std::unique_ptr<tcp_socket>> clients;
    std::vector<std::unique_ptr<tcp_socket>> servers;
    int accepted_count = 0;
    perf::stopwatch sw;

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;

        sw.reset();
        clients.clear();
        servers.clear();
        accepted_count = 0;

        clients.reserve(burst_size);
        servers.reserve(burst_size);

        // Open all client sockets before issuing async operations so a
        // partial failure doesn't leave dangling async_accept operations.
        for( int i = 0; i < burst_size; ++i )
        {
            clients.push_back( std::make_unique<tcp_socket>( ioc.get_executor() ) );
            boost::system::error_code ec;
            ec = clients.back()->open( tcp::v4(), ec );
            if( ec )
            {
                clients.clear();
                asio::post( ioc, [this]() { start(); } );
                return;
            }
            configure_churn_socket( *clients.back() );
        }

        // Initiate all connects and accepts
        for( int i = 0; i < burst_size; ++i )
        {
            clients[i]->async_connect( ep,
                [](boost::system::error_code) {} );

            servers.push_back(std::make_unique<tcp_socket>(ioc.get_executor()));
            acc.async_accept(
                *servers.back(), [this](boost::system::error_code ec) {
                    if (ec)
                        return;
                    ++accepted_count;
                    if (accepted_count == burst_size)
                        close_all();
                });
        }
    }

    void close_all()
    {
        for (auto& c : clients)
            c->close();
        for (auto& s : servers)
            s->close();

        burst_stats.add(sw.elapsed_ns());
        ops.fetch_add(1, std::memory_order_relaxed);
        start();
    }
};

// Burst N connects then accept all -- stresses the listen backlog and
// batched fd creation. Reveals whether the acceptor handles connection
// storms gracefully or suffers from backlog overflow.
void
bench_burst_churn(bench::state& state)
{
    int burst_size = static_cast<int>(state.range(0));
    state.counters["burst_size"] = burst_size;

    asio::io_context ioc;
    auto acc = make_churn_acceptor( ioc );
    auto ep = tcp::endpoint( asio::ip::address_v4::loopback(), acc.local_endpoint().port() );

    std::atomic<bool> running{true};

    burst_churn_op op{ioc,         acc,            ep, running, state.latency(),
                      state.ops(), burst_size,     {}, {},      {},
                      {}};

    perf::stopwatch total_sw;

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
        ioc.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(total_sw.elapsed_seconds());
    acc.close();
}

} // anonymous namespace

bench::benchmark_suite
make_accept_churn_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("accept_churn", F::needs_conntrack_drain)
        .add("sequential", bench_sequential_churn)
        .add("concurrent", bench_concurrent_churn)
            .args({1, 4, 16})
        .add("burst", bench_burst_churn)
            .args({10, 100});
}

} // namespace asio_callback_bench
