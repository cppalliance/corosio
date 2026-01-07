//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_CONFIG_HPP
#define CAPY_CONFIG_HPP

#if defined(__clang__) && !defined(__apple_build_version__) && __clang_major__ >= 20
#define CAPY_CORO_AWAIT_ELIDABLE [[clang::coro_await_elidable]]
#else
#define CAPY_CORO_AWAIT_ELIDABLE
#endif

#endif

