#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

if(NOT TARGET boost_url_test_suite)
    add_subdirectory(../../../url/extra/test_suite test_suite)
endif()

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

if (WolfSSL_FOUND)
    target_link_libraries(boost_corosio_tests PRIVATE boost_corosio_wolfssl)
    target_compile_definitions(boost_corosio_tests PRIVATE BOOST_COROSIO_HAS_WOLFSSL=1)
endif()

if (OpenSSL_FOUND)
    target_link_libraries(boost_corosio_tests PRIVATE boost_corosio_openssl)
    target_compile_definitions(boost_corosio_tests PRIVATE BOOST_COROSIO_HAS_OPENSSL=1)
endif()

# MinGW linker is order-sensitive; system libs must come last
if (MINGW)
    target_link_libraries(boost_corosio_tests PRIVATE ws2_32 crypt32)
endif()

target_include_directories(boost_corosio_tests PRIVATE . ../../)

# Register individual tests with CTest
boost_url_test_suite_discover_tests(boost_corosio_tests)

# Add the main test target to Boost's test suite
add_dependencies(tests boost_corosio_tests)
