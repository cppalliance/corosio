#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Asio benchmark executables for comparison

# io_context benchmark (callbacks)
add_executable(asio_bench_io_context
    io_context_bench.cpp)
target_link_libraries(asio_bench_io_context
    PRIVATE
        Boost::asio
        Threads::Threads)
target_compile_features(asio_bench_io_context PUBLIC cxx_std_20)
target_compile_options(asio_bench_io_context
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
set_property(TARGET asio_bench_io_context
    PROPERTY FOLDER "benchmarks/asio")

# io_context benchmark (coroutines - fair comparison with Corosio)
add_executable(asio_bench_io_context_coro
    io_context_coro_bench.cpp)
target_link_libraries(asio_bench_io_context_coro
    PRIVATE
        Boost::asio
        Threads::Threads)
target_compile_features(asio_bench_io_context_coro PUBLIC cxx_std_20)
target_compile_options(asio_bench_io_context_coro
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
set_property(TARGET asio_bench_io_context_coro
    PROPERTY FOLDER "benchmarks/asio")

# socket throughput benchmark
add_executable(asio_bench_socket_throughput
    socket_throughput_bench.cpp)
target_link_libraries(asio_bench_socket_throughput
    PRIVATE
        Boost::asio
        Threads::Threads)
target_compile_features(asio_bench_socket_throughput PUBLIC cxx_std_20)
target_compile_options(asio_bench_socket_throughput
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
set_property(TARGET asio_bench_socket_throughput
    PROPERTY FOLDER "benchmarks/asio")

# socket latency benchmark
add_executable(asio_bench_socket_latency
    socket_latency_bench.cpp)
target_link_libraries(asio_bench_socket_latency
    PRIVATE
        Boost::asio
        Threads::Threads)
target_compile_features(asio_bench_socket_latency PUBLIC cxx_std_20)
target_compile_options(asio_bench_socket_latency
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
set_property(TARGET asio_bench_socket_latency
    PROPERTY FOLDER "benchmarks/asio")
