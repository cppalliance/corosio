#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

add_subdirectory(client)
add_subdirectory(echo-server)
add_subdirectory(hash-server)
add_subdirectory(nslookup)
add_subdirectory(reconnect)

if(WolfSSL_FOUND)
    add_subdirectory(https-client)
endif()
# Doc-canonical TLS context examples; compiled so the excerpts in
# 3d.tls-context.adoc cannot rot. Exits nonzero without credential
# files, so it is built but not run.
add_executable(corosio_example_tls_context tls_context_examples.cpp)
target_link_libraries(corosio_example_tls_context PRIVATE Boost::corosio)
