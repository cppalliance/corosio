#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Profiler workloads - LTO disabled to preserve call stacks for profiling

function(corosio_add_profile_workload name source)
    add_executable(${name} ${source})
    target_link_libraries(${name}
        PRIVATE
            Boost::corosio
            Threads::Threads)
    set_property(TARGET ${name} PROPERTY FOLDER "benchmarks/profile")
    # Explicitly disable LTO for profiling - preserves function names in profiler output
    set_property(TARGET ${name} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    # Flatten source tree in VS - no nested "Sources" folder
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "" FILES ${source})
endfunction()

corosio_add_profile_workload(profile_coroutine_post coroutine_post_bench.cpp)
corosio_add_profile_workload(profile_scheduler_contention scheduler_contention_bench.cpp)
corosio_add_profile_workload(profile_small_io small_io_bench.cpp)
corosio_add_profile_workload(profile_queue_depth queue_depth_bench.cpp)
corosio_add_profile_workload(profile_concurrent_io concurrent_io_bench.cpp)
