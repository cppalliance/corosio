#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Check LTO support for benchmarks
include(CheckIPOSupported)
check_ipo_supported(RESULT COROSIO_BENCH_LTO_SUPPORTED OUTPUT COROSIO_BENCH_LTO_ERROR LANGUAGES CXX)
if (COROSIO_BENCH_LTO_SUPPORTED)
    message(STATUS "LTO enabled for benchmarks")
else ()
    message(STATUS "LTO not available for benchmarks: ${COROSIO_BENCH_LTO_ERROR}")
endif ()

add_executable(corosio_bench
    main.cpp
    corosio/io_context_bench.cpp
    corosio/socket_throughput_bench.cpp
    corosio/socket_latency_bench.cpp
    corosio/http_server_bench.cpp
    corosio/timer_bench.cpp
    corosio/accept_churn_bench.cpp
    corosio/fan_out_bench.cpp)

target_link_libraries(corosio_bench
    PRIVATE
        Boost::corosio
        Threads::Threads)

target_compile_options(corosio_bench PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)

set_property(TARGET corosio_bench PROPERTY FOLDER "perf/benchmarks")

if (COROSIO_BENCH_LTO_SUPPORTED)
    set_property(TARGET corosio_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (TARGET Boost::asio)
    target_sources(corosio_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/io_context_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/socket_throughput_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/socket_latency_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/http_server_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/io_context_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/socket_throughput_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/socket_latency_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/http_server_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/timer_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/accept_churn_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/coroutine/fan_out_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/timer_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/accept_churn_bench.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/asio/callback/fan_out_bench.cpp)
    target_link_libraries(corosio_bench PRIVATE Boost::asio)
    target_compile_definitions(corosio_bench PRIVATE BOOST_COROSIO_BENCH_HAS_ASIO=1)
endif ()
