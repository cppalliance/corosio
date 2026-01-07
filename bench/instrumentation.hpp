//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef INSTRUMENTATION_HPP
#define INSTRUMENTATION_HPP

// NOLINTBEGIN

#include <cstddef>
#include <cstdlib>
#include <new> // IWYU pragma: export

static std::size_t g_alloc_count = 0;
std::size_t g_io_count = 0;
std::size_t g_work_count = 0;

void* operator new(std::size_t size)
{
    ++g_alloc_count;
    void* p = std::malloc(size);
    if(!p)
        throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

// NOLINTEND

#endif

