#
# Copyright (c) 2026 Steve Gerbino
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Compiled sources for documentation code blocks. The docs include these
# files by tag through the Antora collector, so a fragment that fails to
# build here is a fragment that would render broken on the site.

# Jamfile sets <warnings>extra and <warnings-as-errors>on for this
# directory, and the b2 leg is a hard CI gate. Mirror that posture on the
# doc-test targets so a local CMake build fails on the same warnings; a
# CMake build that cannot fail on warnings is not a check of these files.
# Scoped to this directory: the library and the main test suite keep their
# own settings.
function(boost_corosio_doc_warnings_as_errors target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        # GCC mis-analyzes the structured-binding-from-co_await idiom in
        # coroutine frames; test/doc/Jamfile carries the same exemption.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -Wno-maybe-uninitialized)
        endif()
    endif()
endfunction()

file(GLOB SNIPPETS CONFIGURE_DEPENDS snippets/*.cpp)

# Reference examples: injected into the MrDocs reference by
# doc/addons/extensions/reference-snippets.lua, and compiled here so an example
# in the reference is one the build has already checked. Same target as the
# page snippets -- each file keeps its code in an anonymous namespace, so
# independently authored examples that both define example() do not collide.
#
# They are compiled and never run: several would block on real I/O if executed,
# and some demonstrate a property with static_assert alone. Under coverage that
# makes them worse than useless. Compiling an example instantiates whatever
# header templates it names, and nothing then calls it, so the lines land in
# include/ as instantiated-but-never-entered and read as lost coverage.
#
# Detected from the flags the coverage jobs already pass, so a coverage build
# needs no extra argument of its own. Still an option, so it can be forced
# either way by hand.
#
# This is the guard that matters: every coverage leg corosio runs today builds
# with CMake and passes --coverage through cxxflags, both ci.yml's coverage
# matrix entries and code-coverage.yml. test/doc/Jamfile carries a parallel
# guard defensively, in case a b2 coverage leg is ever added.
string(TOUPPER "${CMAKE_BUILD_TYPE}" BOOST_COROSIO_BUILD_TYPE_UPPER)
# Every route the flag can arrive by: the cache variables, the per-config
# variables, the linker flags (coverage needs it at link time too), and the
# environment, since CMake seeds CMAKE_<LANG>_FLAGS from CXXFLAGS/CFLAGS only on
# the first configure of a fresh tree.
set(BOOST_COROSIO_COVERAGE_FLAGS
    "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS}"
    "${CMAKE_CXX_FLAGS_${BOOST_COROSIO_BUILD_TYPE_UPPER}}"
    "${CMAKE_C_FLAGS_${BOOST_COROSIO_BUILD_TYPE_UPPER}}"
    "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS}"
    "$ENV{CXXFLAGS} $ENV{CFLAGS} $ENV{LDFLAGS}")
if(BOOST_COROSIO_COVERAGE_FLAGS MATCHES
        "(--coverage|-fprofile-arcs|-ftest-coverage|-fprofile-instr-generate)")
    set(BOOST_COROSIO_REFERENCE_SNIPPETS_DEFAULT OFF)
else()
    set(BOOST_COROSIO_REFERENCE_SNIPPETS_DEFAULT ON)
endif()
option(BOOST_COROSIO_BUILD_REFERENCE_SNIPPETS
    "Compile the reference examples in test/doc/reference (off under coverage)"
    ${BOOST_COROSIO_REFERENCE_SNIPPETS_DEFAULT})

if(BOOST_COROSIO_BUILD_REFERENCE_SNIPPETS)
    file(GLOB REFERENCE_SNIPPETS CONFIGURE_DEPENDS reference/*.cpp)
else()
    set(REFERENCE_SNIPPETS)
    message(STATUS
        "[reference-examples] not compiled: they are never executed, and under "
        "coverage they only add never-entered template instantiations")
endif()

set(PFILES ${SNIPPETS} ${REFERENCE_SNIPPETS} doc_warnings.hpp CMakeLists.txt Jamfile)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "" FILES ${PFILES})

add_executable(boost_corosio_doc_tests ${PFILES})
boost_corosio_doc_warnings_as_errors(boost_corosio_doc_tests)
target_link_libraries(
    boost_corosio_doc_tests PRIVATE
    Boost::capy_test_suite_main
    Boost::corosio)
target_include_directories(boost_corosio_doc_tests PRIVATE ../../)
if (WolfSSL_FOUND)
    target_link_libraries(boost_corosio_doc_tests PRIVATE boost_corosio_wolfssl)
    target_compile_definitions(boost_corosio_doc_tests
        PRIVATE BOOST_COROSIO_HAS_WOLFSSL=1)
endif()
# openssl_stream.record.cpp needs the same treatment: its templated
# constructor calls a non-inline member (make_implementation) defined only
# in boost_corosio_openssl, so leaving this target unlinked would break a
# GCC/debug CI leg even though it happens to link here (see the file's own
# comment for how that was verified).
if (OpenSSL_FOUND)
    target_link_libraries(boost_corosio_doc_tests PRIVATE boost_corosio_openssl)
    target_compile_definitions(boost_corosio_doc_tests
        PRIVATE BOOST_COROSIO_HAS_OPENSSL=1)
endif()
boost_capy_test_suite_discover_tests(boost_corosio_doc_tests)
add_dependencies(tests boost_corosio_doc_tests)

# Pages that display a complete program get one executable each; the
# test passes when the program exits cleanly.
#
# Programs demonstrating bugs are built but never executed: running
# them would hang CI or trip sanitizers.
set(COMPILE_ONLY_PROGRAMS
    4k_tcp_server_echo
    index_page_connect
    quick_start_echo)

# Programs register through a POST_BUILD-generated ctest include file
# (the DiscoverTests.cmake scheme) so the test carries the concrete
# binary path and plain `ctest` works in multi-config trees.
file(GLOB DOC_PROGRAMS CONFIGURE_DEPENDS programs/*.cpp)
foreach(src ${DOC_PROGRAMS})
    get_filename_component(name ${src} NAME_WE)
    add_executable(boost_corosio_doc_${name} ${src})
    boost_corosio_doc_warnings_as_errors(boost_corosio_doc_${name})
    target_link_libraries(boost_corosio_doc_${name} PRIVATE Boost::corosio)
    add_dependencies(tests boost_corosio_doc_${name})
    if(name IN_LIST COMPILE_ONLY_PROGRAMS)
        continue()
    endif()

    set(ctest_file ${CMAKE_CURRENT_BINARY_DIR}/${name}_tests.cmake)
    add_custom_command(
        TARGET boost_corosio_doc_${name}
        POST_BUILD
        BYPRODUCTS ${ctest_file}
        COMMAND ${CMAKE_COMMAND}
            -D "TEST_NAME=boost.corosio.doc.${name}"
            -D "TEST_EXECUTABLE=$<TARGET_FILE:boost_corosio_doc_${name}>"
            -D "CTEST_FILE=${ctest_file}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/WriteProgramTest.cmake"
        VERBATIM)

    set(include_file ${CMAKE_CURRENT_BINARY_DIR}/${name}_include.cmake)
    file(WRITE ${include_file}
        "if(EXISTS \"${ctest_file}\")\n"
        "    include(\"${ctest_file}\")\n"
        "else()\n"
        "    add_test(boost.corosio.doc.${name}_NOT_BUILT boost.corosio.doc.${name}_NOT_BUILT)\n"
        "endif()\n")
    set_property(DIRECTORY
        APPEND PROPERTY TEST_INCLUDE_FILES ${include_file})
endforeach()
