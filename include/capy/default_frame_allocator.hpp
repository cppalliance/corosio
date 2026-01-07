//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_DEFAULT_FRAME_ALLOCATOR_HPP
#define CAPY_DEFAULT_FRAME_ALLOCATOR_HPP

#include <cstddef>
#include <new> // IWYU pragma: export

namespace capy {

/** A frame allocator that passes through to global new/delete.

    This allocator provides no pooling or recycling—each allocation
    goes directly to `::operator new` and each deallocation goes to
    `::operator delete`. It serves as a baseline for comparison and
    as a fallback when pooling is not desired.

    @see frame_allocator
    @see frame_pool
*/
struct default_frame_allocator
{
    void* allocate(std::size_t n) { return ::operator new(n); }

    void deallocate(void* p, std::size_t) { ::operator delete(p); }
};

} // namespace capy

#endif
