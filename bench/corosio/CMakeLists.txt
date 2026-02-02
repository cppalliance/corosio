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

function(corosio_add_benchmark name source)
    add_executable(${name} ${source})
    target_link_libraries(${name}
        PRIVATE
            Boost::corosio
            Threads::Threads)
    set_property(TARGET ${name} PROPERTY FOLDER "benchmarks/corosio")
    if (COROSIO_BENCH_LTO_SUPPORTED)
        set_property(TARGET ${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endfunction()

corosio_add_benchmark(corosio_bench_io_context io_context_bench.cpp)
corosio_add_benchmark(corosio_bench_socket_throughput socket_throughput_bench.cpp)
corosio_add_benchmark(corosio_bench_socket_latency socket_latency_bench.cpp)
corosio_add_benchmark(corosio_bench_http_server http_server_bench.cpp)
