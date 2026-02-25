#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Find Boost.Asio for comparison benchmarks (sibling or system-installed).
# This lives here (not in the root CMakeLists.txt) because the Boost
# superproject's dependency scanner greps Boost::* from the root file
# and would pull in Asio's full transitive dependency tree.
if(NOT TARGET Boost::asio)
    find_package(Boost 1.84 QUIET COMPONENTS asio)
    if(TARGET Boost::asio)
        message(STATUS "Found system Boost.Asio -- comparison benchmarks enabled")
    else()
        message(STATUS "Boost.Asio not found -- comparison benchmarks disabled")
    endif()
endif()

# Corosio benchmarks
add_subdirectory(bench)

# Profiler workloads (LTO disabled for call stack visibility)
add_subdirectory(profile)

