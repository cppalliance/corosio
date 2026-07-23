#
# Copyright (c) 2026 Steve Gerbino
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#

# Runs at POST_BUILD with the target's resolved binary path, so the
# generated test carries a concrete path and plain `ctest` works in
# multi-config trees (same scheme as DiscoverTests.cmake).
file(WRITE "${CTEST_FILE}"
    "add_test(\"${TEST_NAME}\" \"${TEST_EXECUTABLE}\")\n"
)
