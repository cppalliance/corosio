#
# Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Corosio benchmarks
add_subdirectory(corosio)

# Asio comparison benchmarks (only if Boost.Asio is available)
if(TARGET Boost::asio)
    add_subdirectory(asio)
endif()
