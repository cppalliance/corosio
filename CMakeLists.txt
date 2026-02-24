#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

cmake_minimum_required(VERSION 3.8...3.31)
set(BOOST_COROSIO_VERSION 1)
if (BOOST_SUPERPROJECT_VERSION)
    set(BOOST_COROSIO_VERSION ${BOOST_SUPERPROJECT_VERSION})
endif ()
project(boost_corosio VERSION "${BOOST_COROSIO_VERSION}" LANGUAGES CXX)
set(BOOST_COROSIO_IS_ROOT OFF)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(BOOST_COROSIO_IS_ROOT ON)
endif ()

if(BOOST_COROSIO_IS_ROOT)
    include(CTest)
endif()
option(BOOST_COROSIO_BUILD_TESTS "Build boost::corosio tests" ${BUILD_TESTING})
option(BOOST_COROSIO_BUILD_PERF "Build boost::corosio performance tools" ${BOOST_COROSIO_IS_ROOT})
option(BOOST_COROSIO_BUILD_EXAMPLES "Build boost::corosio examples" ${BOOST_COROSIO_IS_ROOT})
option(BOOST_COROSIO_MRDOCS_BUILD "Building for MrDocs documentation generation" OFF)

if(NOT TARGET Boost::capy)
    find_package(boost_capy QUIET)
endif()
if(NOT TARGET Boost::capy)
    include(FetchContent)

    # Match capy branch to corosio's current branch when possible
    if(NOT DEFINED CACHE{BOOST_COROSIO_CAPY_TAG})
        execute_process(
            COMMAND git rev-parse --abbrev-ref HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _corosio_branch
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _git_result)
        if(_git_result EQUAL 0 AND _corosio_branch)
            execute_process(
                COMMAND git ls-remote --heads
                    https://github.com/cppalliance/capy.git
                    ${_corosio_branch}
                OUTPUT_VARIABLE _capy_has_branch
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_capy_has_branch)
                set(_default_capy_tag "${_corosio_branch}")
            endif()
        endif()
        if(NOT DEFINED _default_capy_tag)
            set(_default_capy_tag "develop")
        endif()
    endif()
    set(BOOST_COROSIO_CAPY_TAG "${_default_capy_tag}" CACHE STRING
        "Git tag/branch for capy when fetching via FetchContent")

    message(STATUS "Fetching capy...")
    set(BOOST_CAPY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BOOST_CAPY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        capy
        GIT_REPOSITORY https://github.com/cppalliance/capy.git
        GIT_TAG ${BOOST_COROSIO_CAPY_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(capy)
endif()

find_package(Threads REQUIRED)

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

function(boost_corosio_setup_properties target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
    target_include_directories(${target} PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/corosio>)
    target_link_libraries(${target}
        PUBLIC
            Boost::capy
            Threads::Threads
            $<$<PLATFORM_ID:Windows>:ws2_32>)
    target_compile_definitions(${target}
        PUBLIC
            BOOST_COROSIO_NO_LIB
            $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>)
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

if (BOOST_COROSIO_MRDOCS_BUILD)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mrdocs.cpp"
        "#include <boost/corosio.hpp>\n"
        "#include <boost/corosio/native/native.hpp>\n")
    add_library(boost_corosio_mrdocs "${CMAKE_CURRENT_BINARY_DIR}/mrdocs.cpp")
    boost_corosio_setup_properties(boost_corosio_mrdocs)
    target_compile_definitions(boost_corosio_mrdocs PUBLIC BOOST_COROSIO_MRDOCS)
    set_target_properties(boost_corosio_mrdocs PROPERTIES EXPORT_COMPILE_COMMANDS ON)
    return()
endif()

add_library(boost_corosio ${BOOST_COROSIO_HEADERS} ${BOOST_COROSIO_SOURCES})
add_library(Boost::corosio ALIAS boost_corosio)
boost_corosio_setup_properties(boost_corosio)
set_target_properties(boost_corosio PROPERTIES EXPORT_NAME corosio)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(WolfSSL)
# MinGW's linker is single-pass and order-sensitive; system libs must follow
# the static libraries that reference them. Add as interface dependencies so
# CMake's dependency ordering places them after WolfSSL in the link command.
if (MINGW AND TARGET WolfSSL::WolfSSL)
    set_property(TARGET WolfSSL::WolfSSL APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
endif()
if (WolfSSL_FOUND)
    set(BOOST_COROSIO_WOLFSSL_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/wolfssl_stream.hpp")
    file(GLOB_RECURSE BOOST_COROSIO_WOLFSSL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src/*.cpp")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio" PREFIX "include" FILES ${BOOST_COROSIO_WOLFSSL_HEADERS})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/wolfssl/src" PREFIX "src" FILES ${BOOST_COROSIO_WOLFSSL_SOURCES})
    add_library(boost_corosio_wolfssl ${BOOST_COROSIO_WOLFSSL_HEADERS} ${BOOST_COROSIO_WOLFSSL_SOURCES})
    add_library(Boost::corosio_wolfssl ALIAS boost_corosio_wolfssl)
    boost_corosio_setup_properties(boost_corosio_wolfssl)
    set_target_properties(boost_corosio_wolfssl PROPERTIES EXPORT_NAME corosio_wolfssl)
    target_link_libraries(boost_corosio_wolfssl PUBLIC boost_corosio)
    # PUBLIC ensures WolfSSL is linked into final executables (static lib deps don't embed)
    target_link_libraries(boost_corosio_wolfssl PUBLIC WolfSSL::WolfSSL)
    # WolfSSL on Windows needs crypt32 for certificate store access.
    # For MinGW, this is handled via WolfSSL::WolfSSL's interface deps (link order matters).
    if (WIN32 AND NOT MINGW)
        target_link_libraries(boost_corosio_wolfssl PUBLIC crypt32)
    endif ()
    # WolfSSL on macOS uses Apple Security framework for certificate validation
    if (APPLE)
        target_link_libraries(boost_corosio_wolfssl PUBLIC "-framework CoreFoundation" "-framework Security")
    endif ()
    target_compile_definitions(boost_corosio_wolfssl PUBLIC BOOST_COROSIO_HAS_WOLFSSL)
endif ()

find_package(OpenSSL)
# MinGW's linker is single-pass and order-sensitive; system libs must follow
# the static libraries that reference them. Add as interface dependencies so
# CMake's dependency ordering places them after OpenSSL in the link command.
if (MINGW AND TARGET OpenSSL::Crypto)
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ws2_32 crypt32)
endif()
if (OpenSSL_FOUND)
    set(BOOST_COROSIO_OPENSSL_HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/openssl_stream.hpp")
    file(GLOB_RECURSE BOOST_COROSIO_OPENSSL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/src/*.cpp")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio" PREFIX "include" FILES ${BOOST_COROSIO_OPENSSL_HEADERS})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/openssl/src" PREFIX "src" FILES ${BOOST_COROSIO_OPENSSL_SOURCES})
    add_library(boost_corosio_openssl ${BOOST_COROSIO_OPENSSL_HEADERS} ${BOOST_COROSIO_OPENSSL_SOURCES})
    add_library(Boost::corosio_openssl ALIAS boost_corosio_openssl)
    boost_corosio_setup_properties(boost_corosio_openssl)
    set_target_properties(boost_corosio_openssl PROPERTIES EXPORT_NAME corosio_openssl)
    target_link_libraries(boost_corosio_openssl PUBLIC boost_corosio)
    # PUBLIC ensures OpenSSL is linked into final executables (static lib deps don't embed)
    target_link_libraries(boost_corosio_openssl PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    # OpenSSL on Windows needs ws2_32 and crypt32 for socket and cert APIs.
    # For MinGW, this is handled via OpenSSL::Crypto's interface deps (link order matters).
    if (WIN32 AND NOT MINGW)
        target_link_libraries(boost_corosio_openssl PUBLIC ws2_32 crypt32)
    endif ()
    target_compile_definitions(boost_corosio_openssl PUBLIC BOOST_COROSIO_HAS_OPENSSL)
endif ()

# Install
set(_corosio_install_targets boost_corosio)
if(TARGET boost_corosio_openssl)
    list(APPEND _corosio_install_targets boost_corosio_openssl)
endif()
if(TARGET boost_corosio_wolfssl)
    list(APPEND _corosio_install_targets boost_corosio_wolfssl)
endif()

if(BOOST_SUPERPROJECT_VERSION AND NOT CMAKE_VERSION VERSION_LESS 3.13)
    boost_install(
        TARGETS ${_corosio_install_targets}
        VERSION ${BOOST_SUPERPROJECT_VERSION}
        HEADER_DIRECTORY include)
elseif(boost_capy_FOUND)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # Set INSTALL_INTERFACE for standalone installs (boost_install handles
    # this for superproject builds, including versioned-layout paths)
    foreach(_t IN LISTS _corosio_install_targets)
        target_include_directories(${_t} PUBLIC
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    endforeach()

    set(BOOST_COROSIO_INSTALL_CMAKEDIR
        ${CMAKE_INSTALL_LIBDIR}/cmake/boost_corosio)

    install(TARGETS ${_corosio_install_targets}
        EXPORT boost_corosio-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT boost_corosio-targets
        NAMESPACE Boost::
        DESTINATION ${BOOST_COROSIO_INSTALL_CMAKEDIR})

    configure_package_config_file(
        cmake/boost_corosio-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/boost_corosio-config.cmake
        INSTALL_DESTINATION ${BOOST_COROSIO_INSTALL_CMAKEDIR})
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/boost_corosio-config-version.cmake
        COMPATIBILITY SameMajorVersion)

    set(_corosio_config_files
        ${CMAKE_CURRENT_BINARY_DIR}/boost_corosio-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/boost_corosio-config-version.cmake)
    if(WolfSSL_FOUND)
        list(APPEND _corosio_config_files
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindWolfSSL.cmake)
    endif()
    install(FILES ${_corosio_config_files}
        DESTINATION ${BOOST_COROSIO_INSTALL_CMAKEDIR})
endif()

if (BOOST_COROSIO_BUILD_TESTS)
    add_subdirectory(test)
endif ()

if (BOOST_COROSIO_BUILD_EXAMPLES)
    add_subdirectory(example)
endif ()

if (BOOST_COROSIO_BUILD_PERF)
    add_subdirectory(perf)
endif ()
