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

# The liburing target is linked PUBLIC (see CMakeLists.txt) because the
# io_uring scheduler/op headers are reached from public native_*.hpp
# tag-dispatch wrappers and contain inline calls into liburing. The
# imported target is marked IMPORTED_GLOBAL so it propagates out of any
# add_subdirectory() scope into the consuming parent project, matching
# how the PUBLIC link interface is observed there.

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(_liburing QUIET liburing)

    if(_liburing_FOUND)
        set(liburing_VERSION "${_liburing_VERSION}")

        if(NOT TARGET liburing::liburing)
            add_library(liburing::liburing INTERFACE IMPORTED)
            set_target_properties(liburing::liburing
                PROPERTIES IMPORTED_GLOBAL TRUE)
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
