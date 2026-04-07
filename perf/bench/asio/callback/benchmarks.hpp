//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef ASIO_CALLBACK_BENCH_BENCHMARKS_HPP
#define ASIO_CALLBACK_BENCH_BENCHMARKS_HPP

#include "../../common/suite.hpp"

namespace asio_callback_bench {

/// Create the io_context benchmark suite.
bench::benchmark_suite make_io_context_suite();

/// Create the socket throughput benchmark suite.
bench::benchmark_suite make_socket_throughput_suite();

/// Create the socket latency benchmark suite.
bench::benchmark_suite make_socket_latency_suite();

/// Create the HTTP server benchmark suite.
bench::benchmark_suite make_http_server_suite();

/// Create the timer benchmark suite.
bench::benchmark_suite make_timer_suite();

/// Create the accept churn benchmark suite.
bench::benchmark_suite make_accept_churn_suite();

/// Create the fan-out/fan-in benchmark suite.
bench::benchmark_suite make_fan_out_suite();

/// Create the Unix socket throughput benchmark suite.
bench::benchmark_suite make_local_socket_throughput_suite();

/// Create the Unix socket latency benchmark suite.
bench::benchmark_suite make_local_socket_latency_suite();

} // namespace asio_callback_bench

#endif
