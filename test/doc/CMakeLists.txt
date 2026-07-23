#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Compiled sources for documentation code blocks. The docs include these
# files by tag through the Antora collector, so a fragment that fails to
# build here is a fragment that would render broken on the site.

file(GLOB SNIPPETS CONFIGURE_DEPENDS snippets/*.cpp)
set(PFILES ${SNIPPETS} CMakeLists.txt Jamfile)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "" FILES ${PFILES})

add_executable(boost_corosio_doc_tests ${PFILES})
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
