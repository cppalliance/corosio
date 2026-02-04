#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Asio benchmark executable for comparison

add_executable(asio_bench
    main.cpp
    io_context_bench.cpp
    socket_throughput_bench.cpp
    socket_latency_bench.cpp
    http_server_bench.cpp)

target_link_libraries(asio_bench
    PRIVATE
        Boost::asio
        Threads::Threads)

target_compile_features(asio_bench PUBLIC cxx_std_20)

target_compile_options(asio_bench
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)

set_property(TARGET asio_bench PROPERTY FOLDER "benchmarks/asio")

if (COROSIO_BENCH_LTO_SUPPORTED)
    set_property(TARGET asio_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
