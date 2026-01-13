#
# Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Provides imported targets:
#   WolfSSL::WolfSSL

find_path(WolfSSL_INCLUDE_DIR "wolfssl/ssl.h")
find_library(WolfSSL_LIBRARY NAMES "wolfssl" "libwolfssl")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WolfSSL
    REQUIRED_VARS
        WolfSSL_INCLUDE_DIR
        WolfSSL_LIBRARY
)

if(WolfSSL_FOUND)
    add_library(WolfSSL::WolfSSL UNKNOWN IMPORTED)
    set_target_properties(WolfSSL::WolfSSL PROPERTIES
        IMPORTED_LOCATION ${WolfSSL_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${WolfSSL_INCLUDE_DIR})
endif()

mark_as_advanced(
    WolfSSL_INCLUDE_DIR
    WolfSSL_LIBRARY)
