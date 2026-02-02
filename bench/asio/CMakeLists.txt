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

function(asio_add_benchmark name source)
    add_executable(${name} ${source})
    target_link_libraries(${name}
        PRIVATE
            Boost::asio
            Threads::Threads)
    target_compile_features(${name} PUBLIC cxx_std_20)
    target_compile_options(${name}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
    set_property(TARGET ${name} PROPERTY FOLDER "benchmarks/asio")
    if (COROSIO_BENCH_LTO_SUPPORTED)
        set_property(TARGET ${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endfunction()

asio_add_benchmark(asio_bench_io_context io_context_bench.cpp)
asio_add_benchmark(asio_bench_io_context_coro io_context_coro_bench.cpp)
asio_add_benchmark(asio_bench_socket_throughput socket_throughput_bench.cpp)
asio_add_benchmark(asio_bench_socket_latency socket_latency_bench.cpp)
asio_add_benchmark(asio_bench_http_server http_server_bench.cpp)
