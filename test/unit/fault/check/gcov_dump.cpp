//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Links only when the build carries --coverage, since nothing else
// provides libgcov's dump entry point. The test runner supplies main.
extern "C" void __gcov_dump();

void gcov_dump_check()
{
    __gcov_dump();
}
