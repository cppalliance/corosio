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
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace asio_callback_bench {
namespace {

// Echo server: reads then writes back, loops via callbacks
struct echo_server_op : std::enable_shared_from_this<echo_server_op>
{
    tcp::socket& sock;
    char buf[64];

    explicit echo_server_op( tcp::socket& s )
        : sock( s )
    {
    }

    void start()
    {
        do_read();
    }

    void do_read()
    {
        auto self = shared_from_this();
        sock.async_read_some( asio::buffer( buf, 64 ),
            [self]( boost::system::error_code ec, std::size_t n )
            {
                if( ec )
                    return;
                self->do_write( n );
            } );
    }

    void do_write( std::size_t n )
    {
        auto self = shared_from_this();
        asio::async_write( sock, asio::buffer( buf, n ),
            [self]( boost::system::error_code ec, std::size_t )
            {
                if( ec )
                    return;
                self->do_read();
            } );
    }
};

// Single sub-request: write 64 bytes, read 64 bytes, decrement counter
struct sub_request_op : std::enable_shared_from_this<sub_request_op>
{
    tcp::socket& client;
    std::atomic<int>& remaining;
    std::function<void()> on_join;
    char send_buf[64] = {};
    char recv_buf[64];

    sub_request_op( tcp::socket& c, std::atomic<int>& rem,
        std::function<void()> join_cb )
        : client( c )
        , remaining( rem )
        , on_join( std::move( join_cb ) )
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::async_write( client, asio::buffer( send_buf, 64 ),
            [self]( boost::system::error_code ec, std::size_t )
            {
                if( ec )
                {
                    self->finish();
                    return;
                }
                self->do_read();
            } );
    }

    void do_read()
    {
        auto self = shared_from_this();
        asio::async_read( client, asio::buffer( recv_buf, 64 ),
            [self]( boost::system::error_code ec, std::size_t )
            {
                (void)ec;
                self->finish();
            } );
    }

    void finish()
    {
        if( remaining.fetch_sub( 1, std::memory_order_release ) == 1 )
            on_join();
    }
};

struct fork_join_op
{
    asio::io_context& ioc;
    std::vector<tcp::socket>& clients;
    std::vector<tcp::socket>& servers;
    int fan_out;
    std::atomic<bool>& running;
    int64_t& cycles;
    perf::statistics& latency_stats;
    std::atomic<int> remaining{ 0 };
    perf::stopwatch sw;

    void start()
    {
        if( !running.load( std::memory_order_relaxed ) )
        {
            for( auto& c : clients )
                c.close();
            for( auto& s : servers )
                s.close();
            return;
        }

        sw.reset();
        remaining.store( fan_out, std::memory_order_relaxed );

        for( int i = 0; i < fan_out; ++i )
        {
            auto op = std::make_shared<sub_request_op>(
                clients[i], remaining, [this]() { on_join(); } );
            op->start();
        }
    }

    void on_join()
    {
        latency_stats.add( sw.elapsed_us() );
        ++cycles;
        start();
    }
};

// Parent spawns N sub-requests (write+read 64B on pre-connected sockets),
// last sub to complete triggers the next cycle. Compared against the coroutine
// variant, the difference isolates coroutine suspend/resume overhead.
bench::benchmark_result bench_fork_join( int fan_out, double duration_s )
{
    std::cout << "  Fan-out: " << fan_out << "\n";

    asio::io_context ioc;

    std::vector<tcp::socket> clients;
    std::vector<tcp::socket> servers;
    clients.reserve( fan_out );
    servers.reserve( fan_out );

    for( int i = 0; i < fan_out; ++i )
    {
        auto [c, s] = asio_bench::make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int i = 0; i < fan_out; ++i )
    {
        auto echo = std::make_shared<echo_server_op>( servers[i] );
        echo->start();
    }

    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    fork_join_op op{ ioc, clients, servers, fan_out, running, cycles, latency_stats, {}, {} };

    perf::stopwatch total_sw;

    op.start();

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();
    double rate = static_cast<double>( cycles ) / elapsed;

    std::cout << "    Cycles: " << cycles << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( rate ) << "\n";
    perf::print_latency_stats( latency_stats, "Fork-join latency" );
    std::cout << "\n";

    return bench::benchmark_result( "fork_join_" + std::to_string( fan_out ) )
        .add( "fan_out", fan_out )
        .add( "cycles", static_cast<double>( cycles ) )
        .add( "parent_requests_per_sec", rate )
        .add_latency_stats( "fork_join_latency", latency_stats );
}

struct nested_group_op
{
    asio::io_context& ioc;
    std::vector<tcp::socket>& clients;
    int base_idx;
    int n;
    std::atomic<int>& groups_remaining;
    std::function<void()> on_all_groups_done;
    std::atomic<int> subs_remaining;

    nested_group_op( asio::io_context& io, std::vector<tcp::socket>& cli,
        int base, int count, std::atomic<int>& gr,
        std::function<void()> cb )
        : ioc( io )
        , clients( cli )
        , base_idx( base )
        , n( count )
        , groups_remaining( gr )
        , on_all_groups_done( std::move( cb ) )
        , subs_remaining( 0 )
    {
    }

    void start()
    {
        subs_remaining.store( n, std::memory_order_relaxed );
        for( int i = 0; i < n; ++i )
        {
            auto op = std::make_shared<sub_request_op>(
                clients[base_idx + i], subs_remaining,
                [this]() { on_group_done(); } );
            op->start();
        }
    }

    void on_group_done()
    {
        if( groups_remaining.fetch_sub( 1, std::memory_order_release ) == 1 )
            on_all_groups_done();
    }
};

struct nested_op
{
    asio::io_context& ioc;
    std::vector<tcp::socket>& clients;
    std::vector<tcp::socket>& servers;
    int groups;
    int subs_per_group;
    std::atomic<bool>& running;
    int64_t& cycles;
    perf::statistics& latency_stats;
    std::atomic<int> groups_remaining{ 0 };
    std::vector<std::unique_ptr<nested_group_op>> group_ops;
    perf::stopwatch sw;

    void start()
    {
        if( !running.load( std::memory_order_relaxed ) )
        {
            for( auto& c : clients )
                c.close();
            for( auto& s : servers )
                s.close();
            return;
        }

        sw.reset();
        groups_remaining.store( groups, std::memory_order_relaxed );
        group_ops.clear();
        group_ops.reserve( groups );

        for( int g = 0; g < groups; ++g )
        {
            group_ops.push_back( std::make_unique<nested_group_op>(
                ioc, clients, g * subs_per_group,
                subs_per_group, groups_remaining,
                [this]() { on_join(); } ) );
            group_ops.back()->start();
        }
    }

    void on_join()
    {
        latency_stats.add( sw.elapsed_us() );
        ++cycles;
        start();
    }
};

// Two-level fan-out: parent spawns M groups, each group spawns N sub-requests.
// Tests hierarchical coordination cost with pure callbacks — no coroutine
// frames means coordination is driven entirely by atomic counters.
bench::benchmark_result bench_nested(
    int groups, int subs_per_group, double duration_s )
{
    int total_subs = groups * subs_per_group;
    std::cout << "  Groups: " << groups << ", Subs/group: "
              << subs_per_group << " (total " << total_subs << ")\n";

    asio::io_context ioc;

    std::vector<tcp::socket> clients;
    std::vector<tcp::socket> servers;
    clients.reserve( total_subs );
    servers.reserve( total_subs );

    for( int i = 0; i < total_subs; ++i )
    {
        auto [c, s] = asio_bench::make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int i = 0; i < total_subs; ++i )
    {
        auto echo = std::make_shared<echo_server_op>( servers[i] );
        echo->start();
    }

    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    nested_op op{ ioc, clients, servers, groups, subs_per_group,
                  running, cycles, latency_stats, {}, {}, {} };

    perf::stopwatch total_sw;

    op.start();

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();
    double rate = static_cast<double>( cycles ) / elapsed;

    std::cout << "    Cycles: " << cycles << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( rate ) << "\n";
    perf::print_latency_stats( latency_stats, "Nested fan-out latency" );
    std::cout << "\n";

    return bench::benchmark_result(
            "nested_" + std::to_string( groups ) + "x" +
            std::to_string( subs_per_group ) )
        .add( "groups", groups )
        .add( "subs_per_group", subs_per_group )
        .add( "cycles", static_cast<double>( cycles ) )
        .add( "parent_requests_per_sec", rate )
        .add_latency_stats( "nested_latency", latency_stats );
}

// P independent parents each fanning out to N sub-requests on their own
// socket sets. Tests scheduler fairness under competing coordination trees
// and reveals whether per-parent throughput degrades as P grows.
bench::benchmark_result bench_concurrent_parents(
    int num_parents, int fan_out, double duration_s )
{
    std::cout << "  Parents: " << num_parents << ", Fan-out: "
              << fan_out << "\n";

    int total_subs = num_parents * fan_out;
    asio::io_context ioc;

    std::vector<tcp::socket> clients;
    std::vector<tcp::socket> servers;
    clients.reserve( total_subs );
    servers.reserve( total_subs );

    for( int i = 0; i < total_subs; ++i )
    {
        auto [c, s] = asio_bench::make_socket_pair( ioc );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int i = 0; i < total_subs; ++i )
    {
        auto echo = std::make_shared<echo_server_op>( servers[i] );
        echo->start();
    }

    std::atomic<bool> running{ true };
    std::vector<int64_t> cycle_counts( num_parents, 0 );
    std::vector<perf::statistics> stats( num_parents );
    std::atomic<int> parents_done{ 0 };

    struct parent_fork_join_op
    {
        asio::io_context& ioc;
        std::vector<tcp::socket>& clients;
        std::vector<tcp::socket>& servers;
        int base;
        int fan_out;
        int num_parents;
        std::atomic<bool>& running;
        std::atomic<int>& parents_done;
        int64_t& cycles;
        perf::statistics& latency_stats;
        std::atomic<int> remaining;
        perf::stopwatch sw;

        parent_fork_join_op( asio::io_context& io,
            std::vector<tcp::socket>& cli,
            std::vector<tcp::socket>& srv,
            int b, int fo, int np,
            std::atomic<bool>& run,
            std::atomic<int>& pd,
            int64_t& cyc,
            perf::statistics& stats )
            : ioc( io )
            , clients( cli )
            , servers( srv )
            , base( b )
            , fan_out( fo )
            , num_parents( np )
            , running( run )
            , parents_done( pd )
            , cycles( cyc )
            , latency_stats( stats )
            , remaining( 0 )
        {
        }

        void start()
        {
            if( !running.load( std::memory_order_relaxed ) )
            {
                if( parents_done.fetch_add( 1, std::memory_order_acq_rel )
                        == num_parents - 1 )
                {
                    for( auto& c : clients )
                        c.close();
                    for( auto& s : servers )
                        s.close();
                }
                return;
            }

            sw.reset();
            remaining.store( fan_out, std::memory_order_relaxed );

            for( int i = 0; i < fan_out; ++i )
            {
                auto op = std::make_shared<sub_request_op>(
                    clients[base + i], remaining,
                    [this]() { on_join(); } );
                op->start();
            }
        }

        void on_join()
        {
            latency_stats.add( sw.elapsed_us() );
            ++cycles;
            start();
        }
    };

    std::vector<std::unique_ptr<parent_fork_join_op>> parent_ops;
    parent_ops.reserve( num_parents );

    perf::stopwatch total_sw;

    for( int p = 0; p < num_parents; ++p )
    {
        parent_ops.push_back( std::make_unique<parent_fork_join_op>(
            ioc, clients, servers,
            p * fan_out, fan_out, num_parents,
            running, parents_done,
            cycle_counts[p], stats[p] ) );
        parent_ops.back()->start();
    }

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc.run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_cycles = 0;
    for( auto c : cycle_counts )
        total_cycles += c;

    double rate = static_cast<double>( total_cycles ) / elapsed;

    double total_mean = 0;
    double total_p99 = 0;
    for( auto& s : stats )
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Total cycles: " << total_cycles << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision( 3 )
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate( rate ) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency( total_mean / num_parents ) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency( total_p99 / num_parents ) << "\n\n";

    return bench::benchmark_result(
            "concurrent_parents_" + std::to_string( num_parents ) )
        .add( "num_parents", num_parents )
        .add( "fan_out", fan_out )
        .add( "total_cycles", static_cast<double>( total_cycles ) )
        .add( "parent_requests_per_sec", rate )
        .add( "avg_mean_latency_us", total_mean / num_parents )
        .add( "avg_p99_latency_us", total_p99 / num_parents );
}

} // anonymous namespace

void run_fan_out_benchmarks(
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    if( run_all || std::strcmp( filter, "fork_join" ) == 0 )
    {
        perf::print_header( "Fork-Join Fan-Out (Asio Callbacks)" );
        collector.add( bench_fork_join( 1, duration_s ) );
        collector.add( bench_fork_join( 4, duration_s ) );
        collector.add( bench_fork_join( 16, duration_s ) );
        collector.add( bench_fork_join( 64, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "nested" ) == 0 )
    {
        perf::print_header( "Nested Fan-Out (Asio Callbacks)" );
        collector.add( bench_nested( 4, 4, duration_s ) );
        collector.add( bench_nested( 4, 16, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "concurrent_parents" ) == 0 )
    {
        perf::print_header( "Concurrent Parents Fan-Out (Asio Callbacks)" );
        collector.add( bench_concurrent_parents( 1, 16, duration_s ) );
        collector.add( bench_concurrent_parents( 4, 16, duration_s ) );
        collector.add( bench_concurrent_parents( 16, 16, duration_s ) );
    }
}

} // namespace asio_callback_bench
