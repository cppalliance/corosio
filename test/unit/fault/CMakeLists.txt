#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# The POSIX shadows are written against glibc, Darwin libc and the
# FreeBSD libc and the Windows hooks against the PE import table; every
# other platform spells its entry points differently and has no
# coverage here.
if(NOT (WIN32 OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD"))
    return()
endif()

# TSan's runtime defines the same libc symbols the harness shadows and
# models fd ordering inside them, so the interposers never see the call.
# TSan reaches this tree only through CMAKE_CXX_FLAGS: there is no
# sanitizer option of our own to key off.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
    message(STATUS "corosio: fault tests skipped (ThreadSanitizer)")
    return()
endif()

# Windows ASan interposes by patching the very import thunks this
# harness owns, so the two cannot both hold a symbol. The Jamfile gates
# the same configuration off.
if(WIN32 AND CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
    message(STATUS "corosio: fault tests skipped (AddressSanitizer)")
    return()
endif()

file(GLOB FAULT_FILES CONFIGURE_DEPENDS *.cpp *.hpp)
if(WIN32)
    # The POSIX sources include <unistd.h> and friends unconditionally,
    # so on Windows the list is named rather than filtered.
    list(FILTER FAULT_FILES EXCLUDE REGEX "\\.cpp$")
    list(APPEND FAULT_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/fault_arm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/fault_win.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/self_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/win_faults.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/iocp_faults.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/iocp_dissociate_faults.cpp)
else()
    list(FILTER FAULT_FILES EXCLUDE REGEX "fault_win\\.cpp$")
    if(NOT BOOST_COROSIO_HAVE_LIBURING)
        list(FILTER FAULT_FILES EXCLUDE REGEX "uring")
    endif()
endif()
# No per-platform filter beyond those: every backend suite is gated on
# its own BOOST_COROSIO_HAS_* macro, so the ones that do not apply here
# compile away to an empty translation unit.
list(APPEND FAULT_FILES CMakeLists.txt Jamfile)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "" FILES ${FAULT_FILES})

add_executable(boost_corosio_fault_tests ${FAULT_FILES})
target_link_libraries(boost_corosio_fault_tests PRIVATE
    Boost::capy_test_suite_main
    Boost::corosio)
target_include_directories(boost_corosio_fault_tests PRIVATE
    . .. ../../../ ../../../src/corosio)

if(NOT WIN32)
    target_link_libraries(boost_corosio_fault_tests PRIVATE ${CMAKE_DL_LIBS})
    # Interposition only works if the shadows land in the executable's
    # dynamic symbol table. A superproject that builds everything with
    # hidden visibility would strip them, and the readback then refuses
    # to run rather than reporting faults that never fire. Windows
    # rewrites import thunks instead and needs no exported shadows.
    set_target_properties(boost_corosio_fault_tests PROPERTIES
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        ENABLE_EXPORTS ON)
endif()

boost_capy_test_suite_discover_tests(boost_corosio_fault_tests)
add_dependencies(tests boost_corosio_fault_tests)
