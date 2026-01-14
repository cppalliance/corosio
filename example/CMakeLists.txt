#
# Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

add_subdirectory(client)
add_subdirectory(echo-server)
add_subdirectory(nslookup)

if(WolfSSL_FOUND)
    add_subdirectory(https-client)
endif()