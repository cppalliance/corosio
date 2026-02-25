#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# corosio_resolve_deps()
#
# Resolve all build dependencies: sibling Boost libraries when inside a
# boost tree, Capy via find_package / FetchContent, and Threads.
#
# Must be a macro so find_package results (e.g. boost_capy_FOUND) propagate
# to the caller's scope for install logic.
macro(corosio_resolve_deps)
    # Sibling Boost libraries when building standalone inside a boost tree.
    # The Boost::asio reference must stay out of CMakeLists.txt because the
    # superproject's dependency scanner greps for Boost::* literals.
    if(BOOST_COROSIO_IS_ROOT
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/cmake/include/BoostRoot.cmake")
        set(BOOST_INCLUDE_LIBRARIES capy)
        if(BOOST_COROSIO_BUILD_PERF)
            list(APPEND BOOST_INCLUDE_LIBRARIES asio)
        endif()
        set(BOOST_EXCLUDE_LIBRARIES corosio)
        set(CMAKE_FOLDER _deps)
        add_subdirectory(../.. ${CMAKE_CURRENT_BINARY_DIR}/deps/boost EXCLUDE_FROM_ALL)
        unset(CMAKE_FOLDER)
    endif()

    # Capy: prefer already-available target, then find_package, then FetchContent
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
                    ERROR_QUIET
                    TIMEOUT 30)
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
endmacro()

# corosio_setup_mrdocs()
#
# Create boost_corosio_mrdocs, a synthetic translation unit that includes
# all public headers for MrDocs documentation generation.
function(corosio_setup_mrdocs)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mrdocs.cpp"
        "#include <boost/corosio.hpp>\n"
        "#include <boost/corosio/native/native.hpp>\n")
    add_library(boost_corosio_mrdocs "${CMAKE_CURRENT_BINARY_DIR}/mrdocs.cpp")
    target_link_libraries(boost_corosio_mrdocs PUBLIC boost_corosio)
    target_compile_definitions(boost_corosio_mrdocs PUBLIC BOOST_COROSIO_MRDOCS)
    set_target_properties(boost_corosio_mrdocs PROPERTIES EXPORT_COMPILE_COMMANDS ON)
endfunction()

# corosio_find_tls_provider(name
#     PACKAGE      <find_package name>
#     LINK_TARGETS <target> ...
#     MINGW_TARGET <target>
#     MINGW_LIBS   <lib> ...
#     WIN32_LIBS   <lib> ...
#     [FRAMEWORKS  <framework> ...])
#
# Find a TLS provider, apply MinGW link-order workarounds, create a
# boost_corosio_${name} library, link the provider, add platform deps,
# and define BOOST_COROSIO_HAS_<NAME>. Propagates ${PACKAGE}_FOUND to
# the caller's scope.
function(corosio_find_tls_provider name)
    cmake_parse_arguments(_ARGS ""
        "PACKAGE;MINGW_TARGET"
        "LINK_TARGETS;MINGW_LIBS;WIN32_LIBS;FRAMEWORKS" ${ARGN})

    find_package(${_ARGS_PACKAGE})

    # MinGW's linker is single-pass and order-sensitive; system libs must
    # follow the static libraries that reference them
    if(MINGW AND TARGET ${_ARGS_MINGW_TARGET})
        set_property(TARGET ${_ARGS_MINGW_TARGET} APPEND PROPERTY
            INTERFACE_LINK_LIBRARIES ${_ARGS_MINGW_LIBS})
    endif()

    if(${_ARGS_PACKAGE}_FOUND)
        corosio_add_tls_library(${name})
        target_link_libraries(boost_corosio_${name} PUBLIC ${_ARGS_LINK_TARGETS})
        if(WIN32 AND NOT MINGW AND _ARGS_WIN32_LIBS)
            target_link_libraries(boost_corosio_${name} PUBLIC ${_ARGS_WIN32_LIBS})
        endif()
        if(APPLE AND _ARGS_FRAMEWORKS)
            foreach(_fw IN LISTS _ARGS_FRAMEWORKS)
                target_link_libraries(boost_corosio_${name} PUBLIC "-framework ${_fw}")
            endforeach()
        endif()
        string(TOUPPER ${name} _upper_name)
        target_compile_definitions(boost_corosio_${name}
            PUBLIC BOOST_COROSIO_HAS_${_upper_name})
    endif()

    set(${_ARGS_PACKAGE}_FOUND ${${_ARGS_PACKAGE}_FOUND} PARENT_SCOPE)
endfunction()

# corosio_add_tls_library(name)
#
# Create boost_corosio_${name} with standard boilerplate: headers, sources,
# source groups, alias target, and link to boost_corosio (which provides
# all PUBLIC properties transitively). Only PRIVATE properties are set here.
function(corosio_add_tls_library name)
    set(_target boost_corosio_${name})
    set(_headers
        "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio/${name}_stream.hpp")
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${name}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${name}/src/*.cpp")
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/corosio"
        PREFIX "include" FILES ${_headers})
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src/${name}/src"
        PREFIX "src" FILES ${_sources})
    add_library(${_target} ${_headers} ${_sources})
    add_library(Boost::corosio_${name} ALIAS ${_target})
    set_target_properties(${_target} PROPERTIES EXPORT_NAME corosio_${name})
    target_link_libraries(${_target} PUBLIC boost_corosio)
    # PRIVATE properties that don't propagate from boost_corosio
    target_include_directories(${_target} PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/corosio>)
    target_compile_definitions(${_target} PRIVATE BOOST_COROSIO_SOURCE)
    target_compile_options(${_target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-fcoroutines>)
endfunction()

# corosio_install()
#
# Generate install rules for boost_corosio and any TLS variant targets.
# Uses boost_install inside the superproject, standalone CMake packaging
# otherwise.
function(corosio_install)
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
endfunction()
