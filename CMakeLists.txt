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
option(BOOST_COROSIO_BUILD_EXAMPLES "Build boost::corosio examples" ${BOOST_COROSIO_IS_ROOT})
option(BOOST_COROSIO_BUILD_DOCS "Build boost::corosio documentation" OFF)
option(BOOST_COROSIO_MRDOCS_BUILD "Building for MrDocs documentation generation" OFF)

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
# corosio depends on capy, url, and system
set(BOOST_COROSIO_DEPENDENCIES
    Boost::capy
    Boost::url
    Boost::system)

foreach (BOOST_COROSIO_DEPENDENCY ${BOOST_COROSIO_DEPENDENCIES})
    if (BOOST_COROSIO_DEPENDENCY MATCHES "^[ ]*Boost::([A-Za-z0-9_]+)[ ]*$")
        list(APPEND BOOST_COROSIO_INCLUDE_LIBRARIES ${CMAKE_MATCH_1})
    endif ()
endforeach ()

# Complete dependency list
set(BOOST_INCLUDE_LIBRARIES ${BOOST_COROSIO_INCLUDE_LIBRARIES})
set(BOOST_EXCLUDE_LIBRARIES corosio)

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
        find_package(Boost REQUIRED COMPONENTS capy url system)
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
# corosio library
#
#-------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
file(GLOB_RECURSE BOOST_COROSIO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio.hpp")
file(GLOB_RECURSE BOOST_COROSIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

source_group("" FILES "include/boost/corosio.hpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio" PREFIX "include" FILES ${BOOST_COROSIO_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${BOOST_COROSIO_SOURCES})

function(boost_corosio_setup_properties target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${target}
        PUBLIC
            ${BOOST_COROSIO_DEPENDENCIES}
            $<$<PLATFORM_ID:Windows>:ws2_32>)
    target_compile_definitions(${target} PUBLIC BOOST_COROSIO_NO_LIB)
    target_compile_definitions(${target} PRIVATE BOOST_COROSIO_SOURCE)
    if (BUILD_SHARED_LIBS)
        target_compile_definitions(${target} PUBLIC BOOST_COROSIO_DYN_LINK)
    else ()
        target_compile_definitions(${target} PUBLIC BOOST_COROSIO_STATIC_LINK)
    endif ()
    target_compile_options(${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
endfunction()

add_library(boost_corosio ${BOOST_COROSIO_HEADERS} ${BOOST_COROSIO_SOURCES})
add_library(Boost::corosio ALIAS boost_corosio)
boost_corosio_setup_properties(boost_corosio)

#-------------------------------------------------
#
# WolfSSL
#
#-------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(WolfSSL)
if (WolfSSL_FOUND)
    file(GLOB_RECURSE BOOST_COROSIO_WOLFSSL_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/wolfssl/*.hpp")
    file(GLOB_RECURSE BOOST_COROSIO_WOLFSSL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src/*.cpp")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/wolfssl" PREFIX "include" FILES ${BOOST_COROSIO_WOLFSSL_HEADERS})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src" PREFIX "src" FILES ${BOOST_COROSIO_WOLFSSL_SOURCES})
    add_library(boost_corosio_wolfssl ${BOOST_COROSIO_WOLFSSL_HEADERS} ${BOOST_COROSIO_WOLFSSL_SOURCES})
    add_library(Boost::corosio_wolfssl ALIAS boost_corosio_wolfssl)
    boost_corosio_setup_properties(boost_corosio_wolfssl)
    target_link_libraries(boost_corosio_wolfssl PUBLIC boost_corosio)
    target_link_libraries(boost_corosio_wolfssl PRIVATE WolfSSL::WolfSSL)
    target_compile_definitions(boost_corosio_wolfssl PUBLIC BOOST_COROSIO_HAS_WOLFSSL)
endif ()

#-------------------------------------------------
#
# Tests
#
#-------------------------------------------------
if (BOOST_COROSIO_BUILD_TESTS)
    add_subdirectory(test)
endif ()

#-------------------------------------------------
#
# Examples
#
#-------------------------------------------------
if (BOOST_COROSIO_BUILD_EXAMPLES)
    add_subdirectory(example)
endif ()
