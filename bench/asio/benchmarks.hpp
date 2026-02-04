//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef ASIO_BENCH_BENCHMARKS_HPP
#define ASIO_BENCH_BENCHMARKS_HPP

#include "../common/benchmark.hpp"

namespace asio_bench {

/** Run io_context benchmarks.

    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (single_threaded, multithreaded, interleaved, concurrent).
*/
void run_io_context_benchmarks(
    bench::result_collector& collector,
    char const* filter );

/** Run socket throughput benchmarks.

    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (unidirectional, bidirectional).
*/
void run_socket_throughput_benchmarks(
    bench::result_collector& collector,
    char const* filter );

/** Run socket latency benchmarks.

    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (pingpong, concurrent).
*/
void run_socket_latency_benchmarks(
    bench::result_collector& collector,
    char const* filter );

/** Run HTTP server benchmarks.

    @param collector Results collector.
    @param filter Optional filter: nullptr or "all" runs all, or a specific
           benchmark name (single_conn, concurrent, multithread).
*/
void run_http_server_benchmarks(
    bench::result_collector& collector,
    char const* filter );

} // namespace asio_bench

#endif
