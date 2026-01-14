#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0.
# https://www.boost.org/LICENSE_1_0.txt
#

cmake_minimum_required(VERSION 3.16...3.28)

project(cmake_subdir_test LANGUAGES CXX)
set(__ignore__ ${CMAKE_C_COMPILER})
set(__ignore__ ${CMAKE_C_FLAGS})

if(BOOST_CI_INSTALL_TEST)
  find_package(Boost CONFIG REQUIRED COMPONENTS corosio)
else()
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

  set(deps
    # Low-level dependencies (order matters - these must come first)
    assert
    config
    core
    mp11
    predef
    static_assert
    throw_exception
    type_traits
    winapi

    # Mid-level dependencies
    align
    compat
    optional
    variant2

    # Primary dependencies (corosio)
    system
    url
    capy
  )

  foreach(dep IN LISTS deps)
    add_subdirectory(../../../${dep} boostorg/${dep} EXCLUDE_FROM_ALL)
  endforeach()

  # Add corosio last, after all its dependencies
  add_subdirectory(../.. boostorg/corosio)
endif()

add_executable(main main.cpp)
target_link_libraries(main Boost::corosio)
target_compile_features(main PRIVATE cxx_std_20)

enable_testing()
add_test(NAME main COMMAND main)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>)
