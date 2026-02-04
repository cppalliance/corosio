#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

add_executable(corosio_bench
    main.cpp
    io_context_bench.cpp
    socket_throughput_bench.cpp
    socket_latency_bench.cpp
    http_server_bench.cpp)

target_link_libraries(corosio_bench
    PRIVATE
        Boost::corosio
        Threads::Threads)

set_property(TARGET corosio_bench PROPERTY FOLDER "benchmarks/corosio")

if (COROSIO_BENCH_LTO_SUPPORTED)
    set_property(TARGET corosio_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
