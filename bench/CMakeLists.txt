#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
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

# Corosio benchmarks
add_subdirectory(corosio)

# Profiler workloads (LTO disabled for call stack visibility)
add_subdirectory(profile)

# Asio comparison benchmarks (only if Boost.Asio is available)
if(TARGET Boost::asio)
    add_subdirectory(asio)
endif()
