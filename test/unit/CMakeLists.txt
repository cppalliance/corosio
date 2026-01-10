#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

add_subdirectory(../../../url/extra/test_suite test_suite)

file(GLOB_RECURSE PFILES CONFIGURE_DEPENDS *.cpp *.hpp)
list(APPEND PFILES
    CMakeLists.txt
    Jamfile)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX "" FILES ${PFILES})

add_executable(boost_corosio_tests ${PFILES})
target_link_libraries(
    boost_corosio_tests PRIVATE
    boost_url_test_suite_with_main
    Boost::corosio)

target_include_directories(boost_corosio_tests PRIVATE . ../../)

# Register individual tests with CTest
boost_url_test_suite_discover_tests(boost_corosio_tests)

# Add the main test target to Boost's test suite
add_dependencies(tests boost_corosio_tests)
