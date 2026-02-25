#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

cmake_minimum_required(VERSION 3.14...3.31)

if(BOOST_SUPERPROJECT_VERSION)
    set(BOOST_COROSIO_VERSION ${BOOST_SUPERPROJECT_VERSION})
else()
    set(BOOST_COROSIO_VERSION 1)
endif()

project(boost_corosio VERSION "${BOOST_COROSIO_VERSION}" LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BOOST_COROSIO_IS_ROOT ON)
    include(CTest)
else()
    set(BOOST_COROSIO_IS_ROOT OFF)
endif()

option(BOOST_COROSIO_BUILD_TESTS "Build boost::corosio tests" ${BUILD_TESTING})
option(BOOST_COROSIO_BUILD_PERF "Build boost::corosio performance tools" ${BOOST_COROSIO_IS_ROOT})
option(BOOST_COROSIO_BUILD_EXAMPLES "Build boost::corosio examples" ${BOOST_COROSIO_IS_ROOT})
option(BOOST_COROSIO_MRDOCS_BUILD "Building for MrDocs documentation generation" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CorosioBuild)

corosio_resolve_deps()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
file(GLOB_RECURSE BOOST_COROSIO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio.hpp")
file(GLOB_RECURSE BOOST_COROSIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src/*.cpp")

source_group("" FILES "include/boost/corosio.hpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio" PREFIX "include" FILES ${BOOST_COROSIO_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/corosio/src" PREFIX "src" FILES ${BOOST_COROSIO_SOURCES})

add_library(boost_corosio ${BOOST_COROSIO_HEADERS} ${BOOST_COROSIO_SOURCES})
add_library(Boost::corosio ALIAS boost_corosio)
target_compile_features(boost_corosio PUBLIC cxx_std_20)
target_include_directories(boost_corosio
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/corosio>)
target_link_libraries(boost_corosio
    PUBLIC
        Boost::capy
        Threads::Threads
        $<$<PLATFORM_ID:Windows>:ws2_32>)
target_compile_definitions(boost_corosio
    PUBLIC
        BOOST_COROSIO_NO_LIB
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>
        $<IF:$<BOOL:${BUILD_SHARED_LIBS}>,BOOST_COROSIO_DYN_LINK,BOOST_COROSIO_STATIC_LINK>
    PRIVATE
        BOOST_COROSIO_SOURCE)
target_compile_options(boost_corosio
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
set_target_properties(boost_corosio PROPERTIES EXPORT_NAME corosio)

if (BOOST_COROSIO_MRDOCS_BUILD)
    corosio_setup_mrdocs()
    return()
endif()

corosio_find_tls_provider(wolfssl
    PACKAGE      WolfSSL
    LINK_TARGETS WolfSSL::WolfSSL
    MINGW_TARGET WolfSSL::WolfSSL
    MINGW_LIBS   ws2_32 crypt32
    WIN32_LIBS   crypt32
    FRAMEWORKS   CoreFoundation Security)

corosio_find_tls_provider(openssl
    PACKAGE      OpenSSL
    LINK_TARGETS OpenSSL::SSL OpenSSL::Crypto
    MINGW_TARGET OpenSSL::Crypto
    MINGW_LIBS   ws2_32 crypt32
    WIN32_LIBS   ws2_32 crypt32)

corosio_install()

if (BOOST_COROSIO_BUILD_TESTS)
    add_subdirectory(test)
endif ()

if (BOOST_COROSIO_BUILD_EXAMPLES)
    add_subdirectory(example)
endif ()

if (BOOST_COROSIO_BUILD_PERF)
    add_subdirectory(perf)
endif ()
