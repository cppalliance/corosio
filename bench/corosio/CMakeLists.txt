#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Corosio benchmark executables

# io_context benchmark
add_executable(corosio_bench_io_context
    io_context_bench.cpp)
target_link_libraries(corosio_bench_io_context
    PRIVATE
        Boost::corosio
        Threads::Threads)
set_property(TARGET corosio_bench_io_context
    PROPERTY FOLDER "benchmarks/corosio")

# socket throughput benchmark
add_executable(corosio_bench_socket_throughput
    socket_throughput_bench.cpp)
target_link_libraries(corosio_bench_socket_throughput
    PRIVATE
        Boost::corosio
        Threads::Threads)
set_property(TARGET corosio_bench_socket_throughput
    PROPERTY FOLDER "benchmarks/corosio")

# socket latency benchmark
add_executable(corosio_bench_socket_latency
    socket_latency_bench.cpp)
target_link_libraries(corosio_bench_socket_latency
    PRIVATE
        Boost::corosio
        Threads::Threads)
set_property(TARGET corosio_bench_socket_latency
    PROPERTY FOLDER "benchmarks/corosio")
