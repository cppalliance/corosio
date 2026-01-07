#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

cmake_minimum_required(VERSION 3.16)
project(corosio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Threading support
find_package(Threads REQUIRED)

add_library(capy INTERFACE)

target_sources(capy
  PUBLIC
    FILE_SET capy_headers
    TYPE HEADERS
    BASE_DIRS include
    FILES
      include/capy/detail/frame_pool.hpp
      include/capy/detail/root_task.hpp
      include/capy/affine.hpp
      include/capy/async_run.hpp
      include/capy/config.hpp
      include/capy/default_frame_allocator.hpp
      include/capy/executor.hpp
      include/capy/executor_work.hpp
      include/capy/frame_allocator.hpp
      include/capy/make_affine.hpp
      include/capy/run_on.hpp
      include/capy/service_provider.hpp
      include/capy/task.hpp)

add_library(corosio INTERFACE)

target_sources(corosio
  PUBLIC
    FILE_SET corosio_headers
    TYPE HEADERS
    BASE_DIRS include
    FILES
      include/corosio/io_context.hpp
      include/corosio/platform_reactor.hpp
      include/corosio/socket.hpp
      include/corosio/tls_stream.hpp)

target_link_libraries(corosio
  INTERFACE
    capy)

# Benchmark
add_executable(bench)

target_sources(bench
  PRIVATE
    FILE_SET bench_headers
    TYPE HEADERS
    BASE_DIRS bench
    FILES
      bench/bench.hpp
      bench/bench_detail.hpp
      bench/bench_support.hpp
  PRIVATE
    bench/bench.cpp)

target_link_libraries(bench
  PRIVATE
    corosio)

# Warning flags (separate from optimizations)
target_compile_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -Werror -fcoroutines>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

# Optimization flags (separate from warnings)
target_compile_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-O3 -march=native -funroll-loops -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables>
    $<$<CXX_COMPILER_ID:Clang>:-O3 -march=native>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:/O2 /Ob2 /GL>)

target_compile_definitions(bench PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# Use INTERPROCEDURAL_OPTIMIZATION property instead of manual -flto
set_property(TARGET bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# Linker-specific flags that aren't covered by IPO property
target_link_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-s -Wl,--as-needed>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:/LTCG>)

# Clang stdlib requirement
target_compile_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

# Platform Service Provider Test
add_executable(test_service_provider)

target_sources(test_service_provider
  PRIVATE
    test/capy/service_provider.cpp)

target_link_libraries(test_service_provider
  PRIVATE
    capy)

target_compile_options(test_service_provider
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

target_link_options(test_service_provider
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

# Platform Reactor Test
add_executable(test_platform_reactor)

target_sources(test_platform_reactor
  PRIVATE
    test/corosio/platform_reactor.cpp)

target_link_libraries(test_platform_reactor
  PRIVATE
    corosio
    Threads::Threads)

target_compile_options(test_platform_reactor
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

target_link_options(test_platform_reactor
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

# Register as tests for ctest
enable_testing()
add_test(NAME bench COMMAND bench)
add_test(NAME test_service_provider COMMAND test_service_provider)
add_test(NAME test_platform_reactor COMMAND test_platform_reactor)
