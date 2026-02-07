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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace corosio_bench {
namespace {

capy::task<> echo_server(
    corosio::tcp_socket& sock )
{
    char buf[64];
    for( ;; )
    {
        auto [rec, rn] = co_await sock.read_some(
            capy::mutable_buffer( buf, 64 ) );
        if( rec )
            co_return;
        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer( buf, rn ) );
        if( wec )
            co_return;
    }
}

capy::task<> sub_request(
    corosio::tcp_socket& client,
    std::atomic<int>& remaining )
{
    char send_buf[64] = {};
    char recv_buf[64];

    auto [wec, wn] = co_await capy::write(
        client, capy::const_buffer( send_buf, 64 ) );
    if( wec )
    {
        remaining.fetch_sub( 1, std::memory_order_release );
        co_return;
    }

    auto [rec, rn] = co_await capy::read(
        client, capy::mutable_buffer( recv_buf, 64 ) );
    remaining.fetch_sub( 1, std::memory_order_release );
}

// Parent spawns N sub-requests (write+read 64B on pre-connected sockets),
// waits for all N to complete, then repeats. Measures coordination overhead
// as fan-out scales — low throughput points to spawn cost or yield overhead.
bench::benchmark_result bench_fork_join(
    perf::context_factory factory, int fan_out, double duration_s )
{
    std::cout << "  Fan-out: " << fan_out << "\n";

    auto ioc = factory();

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    clients.reserve( fan_out );
    servers.reserve( fan_out );

    for( int i = 0; i < fan_out; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    // Start echo servers
    for( int i = 0; i < fan_out; ++i )
        capy::run_async( ioc->get_executor() )( echo_server( servers[i] ) );

    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    auto parent = [&]() -> capy::task<>
    {
        corosio::timer t( *ioc );
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            std::atomic<int> remaining{ fan_out };
            for( int i = 0; i < fan_out; ++i )
                capy::run_async( ioc->get_executor() )(
                    sub_request( clients[i], remaining ) );

            while( remaining.load( std::memory_order_acquire ) > 0 )
            {
                t.expires_after( std::chrono::nanoseconds( 0 ) );
                auto [ec] = co_await t.wait();
                (void)ec;
            }

            latency_stats.add( sw.elapsed_us() );
            ++cycles;
        }

        // Close sockets to unblock echo servers
        for( auto& c : clients )
            c.close();
        for( auto& s : servers )
            s.close();
    };

    perf::stopwatch total_sw;

    capy::run_async( ioc->get_executor() )( parent() );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
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

// Two-level fan-out: parent spawns M groups, each group spawns N sub-requests.
// Tests hierarchical coordination cost — the extra indirection layer adds
// spawn and join overhead beyond flat fork-join.
bench::benchmark_result bench_nested(
    perf::context_factory factory, int groups, int subs_per_group,
    double duration_s )
{
    int total_subs = groups * subs_per_group;
    std::cout << "  Groups: " << groups << ", Subs/group: "
              << subs_per_group << " (total " << total_subs << ")\n";

    auto ioc = factory();

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    clients.reserve( total_subs );
    servers.reserve( total_subs );

    for( int i = 0; i < total_subs; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int i = 0; i < total_subs; ++i )
        capy::run_async( ioc->get_executor() )( echo_server( servers[i] ) );

    std::atomic<bool> running{ true };
    int64_t cycles = 0;
    perf::statistics latency_stats;

    auto group_task = [&](
        int base_idx, int n, std::atomic<int>& groups_remaining ) -> capy::task<>
    {
        std::atomic<int> subs_remaining{ n };
        for( int i = 0; i < n; ++i )
            capy::run_async( ioc->get_executor() )(
                sub_request( clients[base_idx + i], subs_remaining ) );

        corosio::timer t( *ioc );
        while( subs_remaining.load( std::memory_order_acquire ) > 0 )
        {
            t.expires_after( std::chrono::nanoseconds( 0 ) );
            auto [ec] = co_await t.wait();
            (void)ec;
        }

        groups_remaining.fetch_sub( 1, std::memory_order_release );
    };

    auto parent = [&]() -> capy::task<>
    {
        corosio::timer t( *ioc );
        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            std::atomic<int> groups_remaining{ groups };
            for( int g = 0; g < groups; ++g )
                capy::run_async( ioc->get_executor() )(
                    group_task( g * subs_per_group, subs_per_group,
                        groups_remaining ) );

            while( groups_remaining.load( std::memory_order_acquire ) > 0 )
            {
                t.expires_after( std::chrono::nanoseconds( 0 ) );
                auto [ec] = co_await t.wait();
                (void)ec;
            }

            latency_stats.add( sw.elapsed_us() );
            ++cycles;
        }

        for( auto& c : clients )
            c.close();
        for( auto& s : servers )
            s.close();
    };

    perf::stopwatch total_sw;

    capy::run_async( ioc->get_executor() )( parent() );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
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
    perf::context_factory factory, int num_parents, int fan_out,
    double duration_s )
{
    std::cout << "  Parents: " << num_parents << ", Fan-out: "
              << fan_out << "\n";

    int total_subs = num_parents * fan_out;
    auto ioc = factory();

    std::vector<corosio::tcp_socket> clients;
    std::vector<corosio::tcp_socket> servers;
    clients.reserve( total_subs );
    servers.reserve( total_subs );

    for( int i = 0; i < total_subs; ++i )
    {
        auto [c, s] = corosio::test::make_socket_pair( *ioc );
        c.set_no_delay( true );
        s.set_no_delay( true );
        clients.push_back( std::move( c ) );
        servers.push_back( std::move( s ) );
    }

    for( int i = 0; i < total_subs; ++i )
        capy::run_async( ioc->get_executor() )( echo_server( servers[i] ) );

    std::atomic<bool> running{ true };
    std::vector<int64_t> cycle_counts( num_parents, 0 );
    std::vector<perf::statistics> stats( num_parents );

    std::atomic<int> parents_done{ 0 };

    auto parent_task = [&]( int parent_idx ) -> capy::task<>
    {
        int base = parent_idx * fan_out;
        corosio::timer t( *ioc );

        while( running.load( std::memory_order_relaxed ) )
        {
            perf::stopwatch sw;

            std::atomic<int> remaining{ fan_out };
            for( int i = 0; i < fan_out; ++i )
                capy::run_async( ioc->get_executor() )(
                    sub_request( clients[base + i], remaining ) );

            while( remaining.load( std::memory_order_acquire ) > 0 )
            {
                t.expires_after( std::chrono::nanoseconds( 0 ) );
                auto [ec] = co_await t.wait();
                (void)ec;
            }

            stats[parent_idx].add( sw.elapsed_us() );
            ++cycle_counts[parent_idx];
        }

        // Last parent to exit closes all sockets
        if( parents_done.fetch_add( 1, std::memory_order_acq_rel )
                == num_parents - 1 )
        {
            for( auto& c : clients )
                c.close();
            for( auto& s : servers )
                s.close();
        }
    };

    perf::stopwatch total_sw;

    for( int p = 0; p < num_parents; ++p )
        capy::run_async( ioc->get_executor() )( parent_task( p ) );

    std::thread stopper( [&]()
    {
        std::this_thread::sleep_for(
            std::chrono::duration<double>( duration_s ) );
        running.store( false, std::memory_order_relaxed );
    } );

    ioc->run();
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
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s )
{
    bool run_all = !filter || std::strcmp( filter, "all" ) == 0;

    if( run_all || std::strcmp( filter, "fork_join" ) == 0 )
    {
        perf::print_header( "Fork-Join Fan-Out (Corosio)" );
        collector.add( bench_fork_join( factory, 1, duration_s ) );
        collector.add( bench_fork_join( factory, 4, duration_s ) );
        collector.add( bench_fork_join( factory, 16, duration_s ) );
        collector.add( bench_fork_join( factory, 64, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "nested" ) == 0 )
    {
        perf::print_header( "Nested Fan-Out (Corosio)" );
        collector.add( bench_nested( factory, 4, 4, duration_s ) );
        collector.add( bench_nested( factory, 4, 16, duration_s ) );
    }

    if( run_all || std::strcmp( filter, "concurrent_parents" ) == 0 )
    {
        perf::print_header( "Concurrent Parents Fan-Out (Corosio)" );
        collector.add( bench_concurrent_parents( factory, 1, 16, duration_s ) );
        collector.add( bench_concurrent_parents( factory, 4, 16, duration_s ) );
        collector.add( bench_concurrent_parents( factory, 16, 16, duration_s ) );
    }
}

} // namespace corosio_bench
