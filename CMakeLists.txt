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

#------------------------------------------------------------------------------
# capy library
#------------------------------------------------------------------------------

file(GLOB_RECURSE CAPY_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/capy/*.hpp")
file(GLOB_RECURSE CAPY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capy/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capy/src/*.cpp")

add_library(capy STATIC)

target_sources(capy
    PUBLIC
        FILE_SET capy_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES ${CAPY_HEADERS}
    PRIVATE
        ${CAPY_SOURCES})

target_include_directories(capy
    PUBLIC
        include
    PRIVATE
        src/capy)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/capy" PREFIX "capy" FILES ${CAPY_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/capy/src" PREFIX "src" FILES ${CAPY_SOURCES})

#------------------------------------------------------------------------------
# corosio library
#------------------------------------------------------------------------------

file(GLOB_RECURSE COROSIO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/corosio/*.hpp")
file(GLOB_RECURSE COROSIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src/*.cpp")

add_library(corosio STATIC)

target_sources(corosio
    PUBLIC
        FILE_SET corosio_headers
        TYPE HEADERS
        BASE_DIRS include
        FILES ${COROSIO_HEADERS}
    PRIVATE
        ${COROSIO_SOURCES})

target_include_directories(corosio
    PUBLIC
        include
    PRIVATE
        src/corosio)

target_link_libraries(corosio
    PUBLIC
        capy)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/corosio" PREFIX "corosio" FILES ${COROSIO_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src" PREFIX "src" FILES ${COROSIO_SOURCES})

#------------------------------------------------------------------------------
# Benchmark
#------------------------------------------------------------------------------

file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")

add_executable(bench)

target_sources(bench
    PRIVATE
        ${BENCH_SOURCES})

target_include_directories(bench PRIVATE bench)

target_link_libraries(bench
    PRIVATE
        corosio)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/bench" PREFIX "bench" FILES ${BENCH_SOURCES})

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

#------------------------------------------------------------------------------
# Service Provider Test
#------------------------------------------------------------------------------

add_executable(test_service_provider)

target_sources(test_service_provider
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/capy/service_provider.cpp")

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

# async_run Test
add_executable(test_async_run)

target_sources(test_async_run
  PRIVATE
    test/capy/async_run.cpp)

target_link_libraries(test_async_run
  PRIVATE
    capy)

target_compile_options(test_async_run
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

target_link_options(test_async_run
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

#------------------------------------------------------------------------------
# Task Test
#------------------------------------------------------------------------------

add_executable(test_task)

target_sources(test_task
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/capy/task.cpp")

target_link_libraries(test_task
    PRIVATE
        capy)

target_compile_options(test_task
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

target_link_options(test_task
    PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

#------------------------------------------------------------------------------
# Platform Reactor Test
#------------------------------------------------------------------------------

add_executable(test_platform_reactor)

target_sources(test_platform_reactor
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/corosio/platform_reactor.cpp")

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

# Socket Test
add_executable(test_socket)

target_sources(test_socket
  PRIVATE
    test/corosio/socket.cpp)

target_link_libraries(test_socket
  PRIVATE
    corosio
    Threads::Threads)

target_compile_options(test_socket
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)

target_link_options(test_socket
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

# Register as tests for ctest
#------------------------------------------------------------------------------

enable_testing()
add_test(NAME bench COMMAND bench)
add_test(NAME test_service_provider COMMAND test_service_provider)
add_test(NAME test_async_run COMMAND test_async_run)
add_test(NAME test_platform_reactor COMMAND test_platform_reactor)
add_test(NAME test_socket COMMAND test_socket)
add_test(NAME test_task COMMAND test_task)
