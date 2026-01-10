#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

#-------------------------------------------------
#
# Project
#
#-------------------------------------------------
cmake_minimum_required(VERSION 3.16...3.28)
set(BOOST_COROSIO_VERSION 1)
if (BOOST_SUPERPROJECT_VERSION)
    set(BOOST_COROSIO_VERSION ${BOOST_SUPERPROJECT_VERSION})
endif ()
project(boost_corosio VERSION "${BOOST_COROSIO_VERSION}" LANGUAGES CXX)
set(BOOST_COROSIO_IS_ROOT OFF)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BOOST_COROSIO_IS_ROOT ON)
endif ()
set(__ignore__ ${CMAKE_C_COMPILER})

#-------------------------------------------------
#
# Options
#
#-------------------------------------------------
if (BOOST_COROSIO_IS_ROOT)
    include(CTest)
endif ()
option(BOOST_COROSIO_BUILD_TESTS "Build boost::corosio tests" ${BUILD_TESTING})
option(BOOST_COROSIO_BUILD_BENCH "Build boost::corosio benchmarks" ${BOOST_COROSIO_IS_ROOT})

# Check if environment variable BOOST_SRC_DIR is set
if (NOT DEFINED BOOST_SRC_DIR AND DEFINED ENV{BOOST_SRC_DIR})
    set(DEFAULT_BOOST_SRC_DIR "$ENV{BOOST_SRC_DIR}")
else ()
    set(DEFAULT_BOOST_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
endif ()
set(BOOST_SRC_DIR ${DEFAULT_BOOST_SRC_DIR} CACHE STRING "Boost source dir to use when running CMake from this directory")

#-------------------------------------------------
#
# Boost modules
#
#-------------------------------------------------
# corosio depends on buffers
set(BOOST_COROSIO_DEPENDENCIES
    Boost::buffers)

foreach (BOOST_COROSIO_DEPENDENCY ${BOOST_COROSIO_DEPENDENCIES})
    if (BOOST_COROSIO_DEPENDENCY MATCHES "^[ ]*Boost::([A-Za-z0-9_]+)[ ]*$")
        list(APPEND BOOST_COROSIO_INCLUDE_LIBRARIES ${CMAKE_MATCH_1})
    endif ()
endforeach ()

# Complete dependency list
set(BOOST_INCLUDE_LIBRARIES ${BOOST_COROSIO_INCLUDE_LIBRARIES})
# Exclude both capy and corosio since this repo provides them
set(BOOST_EXCLUDE_LIBRARIES capy corosio)

#-------------------------------------------------
#
# Add Boost Subdirectory
#
#-------------------------------------------------
if (BOOST_COROSIO_IS_ROOT)
    set(CMAKE_FOLDER Dependencies)
    # Find absolute BOOST_SRC_DIR
    if (NOT IS_ABSOLUTE ${BOOST_SRC_DIR})
        set(BOOST_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${BOOST_SRC_DIR}")
    endif ()

    # Validate BOOST_SRC_DIR
    set(BOOST_SRC_DIR_IS_VALID ON)
    foreach (F "CMakeLists.txt" "Jamroot" "boost-build.jam" "bootstrap.sh" "libs")
        if (NOT EXISTS "${BOOST_SRC_DIR}/${F}")
            message(STATUS "${BOOST_SRC_DIR}/${F} does not exist. Fallback to find_package.")
            set(BOOST_SRC_DIR_IS_VALID OFF)
            break()
        endif ()
    endforeach ()

    # Create Boost interface targets
    if (BOOST_SRC_DIR_IS_VALID)
        # From BOOST_SRC_DIR
        if (BUILD_SHARED_LIBS)
            set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        endif ()
        set(PREV_BUILD_TESTING ${BUILD_TESTING})
        set(BUILD_TESTING OFF CACHE BOOL "Build the tests." FORCE)
        add_subdirectory(${BOOST_SRC_DIR} Dependencies/boost EXCLUDE_FROM_ALL)
        set(BUILD_TESTING ${PREV_BUILD_TESTING} CACHE BOOL "Build the tests." FORCE)
        set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${BOOST_SRC_DIR}/tools/cmake/include")
    else ()
        # From Boost Package
        find_package(Boost REQUIRED COMPONENTS buffers)
        foreach (BOOST_INCLUDE_LIBRARY ${BOOST_COROSIO_INCLUDE_LIBRARIES})
            if (NOT TARGET Boost::${BOOST_INCLUDE_LIBRARY})
                add_library(Boost::${BOOST_INCLUDE_LIBRARY} ALIAS Boost::headers)
            endif ()
        endforeach ()
    endif ()
    unset(CMAKE_FOLDER)
endif ()

#-------------------------------------------------
#
# Threading support
#
#-------------------------------------------------
find_package(Threads REQUIRED)

#-------------------------------------------------
#
# capy library
#
#-------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

file(GLOB_RECURSE BOOST_CAPY_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/capy/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/capy.hpp")
file(GLOB_RECURSE BOOST_CAPY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capy/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capy/*.cpp")

source_group("" FILES "include/boost/capy.hpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/capy" PREFIX "include" FILES ${BOOST_CAPY_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/capy" PREFIX "src" FILES ${BOOST_CAPY_SOURCES})

function(boost_capy_setup_properties target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
endfunction()

add_library(boost_capy ${BOOST_CAPY_HEADERS} ${BOOST_CAPY_SOURCES})
add_library(Boost::capy ALIAS boost_capy)
boost_capy_setup_properties(boost_capy)

#-------------------------------------------------
#
# corosio library
#
#-------------------------------------------------
file(GLOB_RECURSE BOOST_COROSIO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio.hpp")
file(GLOB_RECURSE BOOST_COROSIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/*.cpp")

source_group("" FILES "include/boost/corosio.hpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio" PREFIX "include" FILES ${BOOST_COROSIO_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio" PREFIX "src" FILES ${BOOST_COROSIO_SOURCES})

function(boost_corosio_setup_properties target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
    target_link_libraries(${target}
        PUBLIC
            Boost::capy
            ${BOOST_COROSIO_DEPENDENCIES})
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
endfunction()

add_library(boost_corosio ${BOOST_COROSIO_HEADERS} ${BOOST_COROSIO_SOURCES})
add_library(Boost::corosio ALIAS boost_corosio)
boost_corosio_setup_properties(boost_corosio)

#-------------------------------------------------
#
# Tests
#
#-------------------------------------------------
if (BOOST_COROSIO_BUILD_TESTS)
    # Common test setup function
    function(boost_corosio_add_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name}
            PRIVATE
                Boost::corosio
                Threads::Threads)
        target_compile_options(${name}
            PRIVATE
                $<$<CXX_COMPILER_ID:GNU>:-fcoroutines -Wall -Wextra -Wpedantic>
                $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++ -Wall -Wextra -Wpedantic>
                $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->)
        target_link_options(${name}
            PRIVATE
                $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    # capy tests
    boost_corosio_add_test(test_service_provider test/capy/service_provider.cpp)
    boost_corosio_add_test(test_async_run test/capy/async_run.cpp)
    boost_corosio_add_test(test_task test/capy/task.cpp)
    boost_corosio_add_test(test_run_on test/capy/run_on.cpp)

    # corosio tests
    boost_corosio_add_test(test_platform_reactor test/corosio/platform_reactor.cpp)
    boost_corosio_add_test(test_socket test/corosio/socket.cpp)
endif ()

#-------------------------------------------------
#
# Benchmarks
#
#-------------------------------------------------
if (BOOST_COROSIO_BUILD_BENCH)
    file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.cpp")

    add_executable(bench ${BENCH_SOURCES})

    target_include_directories(bench PRIVATE bench)

    target_link_libraries(bench
        PRIVATE
            Boost::corosio)

    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/bench" PREFIX "bench" FILES ${BENCH_SOURCES})

    # Warning flags
    target_compile_options(bench
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic -fcoroutines>
            $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->)

    # Optimization flags
    target_compile_options(bench
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-O3 -march=native -funroll-loops -ffast-math -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables>
            $<$<CXX_COMPILER_ID:Clang>:-O3 -march=native>
            $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:/O2 /Ob2 /GL /arch:AVX2 /Qspectre->)

    target_compile_definitions(bench PRIVATE
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

    # Use INTERPROCEDURAL_OPTIMIZATION property instead of manual -flto
    set_property(TARGET bench PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)

    # Linker-specific flags
    target_link_options(bench
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-s -Wl,--as-needed>
            $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>
            $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:/LTCG>)

    # Clang stdlib requirement
    target_compile_options(bench
        PRIVATE
            $<$<CXX_COMPILER_ID:Clang>:-stdlib=libc++>)

    add_test(NAME bench COMMAND bench)
endif ()
