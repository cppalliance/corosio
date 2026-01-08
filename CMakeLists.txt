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

# IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ==============================================================================
# Libraries
# ==============================================================================

add_library(capy INTERFACE)

file(GLOB_RECURSE CAPY_HEADERS CONFIGURE_DEPENDS include/capy/*.hpp)

target_sources(capy
  PUBLIC
    FILE_SET capy_headers
    TYPE HEADERS
    BASE_DIRS include
    FILES
      ${CAPY_HEADERS})

add_library(corosio INTERFACE)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include/capy PREFIX "capy" FILES ${CAPY_HEADERS})


file(GLOB_RECURSE COROSIO_HEADERS CONFIGURE_DEPENDS include/corosio/*.hpp)

target_sources(corosio
  PUBLIC
    FILE_SET corosio_headers
    TYPE HEADERS
    BASE_DIRS include
    FILES
     ${COROSIO_HEADERS})

target_link_libraries(corosio
  INTERFACE
    capy)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include/corosio PREFIX "corosio" FILES ${COROSIO_HEADERS})

# ==============================================================================
# Compiler/Linker Flag Configuration
# ==============================================================================

# Diagnostic flags (warnings treated as errors)
set(DIAGNOSTIC_FLAGS_GNU -Wall -Wextra -Wpedantic -Werror -fcoroutines)
set(DIAGNOSTIC_FLAGS_CLANG -Wall -Wextra -Wpedantic -Werror)
set(DIAGNOSTIC_FLAGS_MSVC /W4 /WX /permissive-)

# Optimization flags for benchmarks
set(OPTIMIZATION_FLAGS_GNU -O3 -march=native -funroll-loops -ffast-math)
set(OPTIMIZATION_FLAGS_CLANG -O3 -march=native)
set(OPTIMIZATION_FLAGS_MSVC /O2 /Ob2)

# Size reduction flags (remove stack protection and unwind tables for benchmarks)
set(SIZE_REDUCTION_FLAGS -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables)

# Clang standard library flags (required for both compile and link)
set(CLANG_STDLIB_FLAGS -stdlib=libc++)

# Linker flags
set(LINK_FLAGS_GNU -s -Wl,--as-needed)
set(LINK_FLAGS_CLANG ${CLANG_STDLIB_FLAGS})

# ==============================================================================
# Benchmark Executable
# ==============================================================================

add_executable(bench)

file(GLOB_RECURSE BENCH_HEADERS CONFIGURE_DEPENDS bench/*.hpp)
file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS bench/*.cpp)

target_sources(bench
  PRIVATE
    FILE_SET bench_headers
    TYPE HEADERS
    BASE_DIRS bench
    FILES
      ${BENCH_HEADERS}
  PRIVATE
    ${BENCH_SOURCES})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/bench PREFIX "bench" FILES ${BENCH_HEADERS} ${BENCH_SOURCES})

target_link_libraries(bench
  PRIVATE
    corosio)

# Diagnostic flags
target_compile_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:${DIAGNOSTIC_FLAGS_GNU}>
    $<$<CXX_COMPILER_ID:Clang>:${DIAGNOSTIC_FLAGS_CLANG}>
    $<$<CXX_COMPILER_ID:MSVC>:${DIAGNOSTIC_FLAGS_MSVC}>)

# Optimization flags (only for Release-type builds)
target_compile_options(bench
  PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:${OPTIMIZATION_FLAGS_GNU} ${SIZE_REDUCTION_FLAGS}>
    $<$<AND:$<CXX_COMPILER_ID:Clang>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:${OPTIMIZATION_FLAGS_CLANG} ${SIZE_REDUCTION_FLAGS}>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:${OPTIMIZATION_FLAGS_MSVC}>)

# Clang requires -stdlib=libc++ for both compile and link
target_compile_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:${CLANG_STDLIB_FLAGS}>)

target_compile_definitions(bench PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# Link-time optimization (cross-platform via CMake property)
set_property(TARGET bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

# Additional linker flags
target_link_options(bench
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:${LINK_FLAGS_GNU}>
    $<$<CXX_COMPILER_ID:Clang>:${LINK_FLAGS_CLANG}>)

# ==============================================================================
# Test Executables
# ==============================================================================

# Platform Service Provider Test -----------------------------------------------

add_executable(test_service_provider)

file(GLOB_RECURSE TEST_CAPY_SOURCES CONFIGURE_DEPENDS test/capy/*.cpp)

target_sources(test_service_provider
  PRIVATE
    ${TEST_CAPY_SOURCES})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/test/capy PREFIX "capy (tests)" FILES ${TEST_CAPY_SOURCES})

target_link_libraries(test_service_provider
  PRIVATE
    capy)

target_compile_options(test_service_provider
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:${DIAGNOSTIC_FLAGS_GNU}>
    $<$<CXX_COMPILER_ID:Clang>:${DIAGNOSTIC_FLAGS_CLANG}>
    $<$<CXX_COMPILER_ID:MSVC>:${DIAGNOSTIC_FLAGS_MSVC}>)

target_compile_options(test_service_provider
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:${CLANG_STDLIB_FLAGS}>)

target_link_options(test_service_provider
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:${LINK_FLAGS_CLANG}>)

# Platform Reactor Test --------------------------------------------------------

add_executable(test_platform_reactor)

file(GLOB_RECURSE TEST_COROSIO_SOURCES CONFIGURE_DEPENDS test/corosio/*.cpp)

target_sources(test_platform_reactor
  PRIVATE
    ${TEST_COROSIO_SOURCES})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/test/corosio PREFIX "corosio (tests)" FILES ${TEST_COROSIO_SOURCES})

target_link_libraries(test_platform_reactor
  PRIVATE
    corosio
    Threads::Threads)

target_compile_options(test_platform_reactor
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:${DIAGNOSTIC_FLAGS_GNU}>
    $<$<CXX_COMPILER_ID:Clang>:${DIAGNOSTIC_FLAGS_CLANG}>
    $<$<CXX_COMPILER_ID:MSVC>:${DIAGNOSTIC_FLAGS_MSVC}>)

target_compile_options(test_platform_reactor
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:${CLANG_STDLIB_FLAGS}>)

target_link_options(test_platform_reactor
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:${LINK_FLAGS_CLANG}>)

# CTest registration -----------------------------------------------------------
enable_testing()
add_test(NAME bench COMMAND bench)
add_test(NAME test_service_provider COMMAND test_service_provider)
add_test(NAME test_platform_reactor COMMAND test_platform_reactor)
