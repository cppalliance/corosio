//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef COROSIO_BENCH_BENCHMARKS_HPP
#define COROSIO_BENCH_BENCHMARKS_HPP

#include "../../common/backend_selection.hpp"
#include "../common/benchmark.hpp"

namespace corosio_bench {

/** Run io_context benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (single_threaded, multithreaded, interleaved, concurrent).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_io_context_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run socket throughput benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (unidirectional, bidirectional).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_socket_throughput_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run socket latency benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (pingpong, concurrent).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_socket_latency_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run HTTP server benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (single_conn, concurrent, multithread).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_http_server_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run timer benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (schedule_cancel, fire_rate, concurrent).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_timer_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run accept churn benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (sequential, concurrent, burst).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_accept_churn_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

/** Run fan-out/fan-in benchmarks using the given context factory.

    @param factory Factory that creates a fresh io_context.
    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (fork_join, nested, concurrent_parents).
    @param duration_s Duration in seconds for each benchmark.
*/
void run_fan_out_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s );

} // namespace corosio_bench

#endif
