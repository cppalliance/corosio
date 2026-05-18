#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Find liburing via pkg-config and expose an imported target liburing::liburing.
# Sets: liburing_FOUND, liburing_VERSION

# Note: this Find module is intentionally NOT installed alongside
# boost_corosio-config.cmake. The liburing target is linked PRIVATE
# (see CMakeLists.txt) and the BOOST_COROSIO_HAVE_LIBURING macro
# carries no link obligation, so consumers do not need to find liburing.
# If io_uring types are ever exposed in public headers, register this
# file in corosio_install() and add find_dependency(liburing) to the
# package config template (see how WolfSSL is handled).

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(_liburing QUIET liburing)

    if(_liburing_FOUND)
        set(liburing_VERSION "${_liburing_VERSION}")

        if(NOT TARGET liburing::liburing)
            add_library(liburing::liburing INTERFACE IMPORTED)
            target_include_directories(liburing::liburing
                INTERFACE ${_liburing_INCLUDE_DIRS})
            target_link_libraries(liburing::liburing
                INTERFACE ${_liburing_LINK_LIBRARIES})
            target_compile_options(liburing::liburing
                INTERFACE ${_liburing_CFLAGS_OTHER})
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(liburing
    REQUIRED_VARS _liburing_FOUND
    VERSION_VAR   liburing_VERSION)
