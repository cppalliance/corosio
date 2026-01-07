//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_FRAME_ALLOCATOR_HPP
#define CAPY_FRAME_ALLOCATOR_HPP

#include <capy/config.hpp>
#include <concepts>
#include <cstddef>

namespace capy {

/** A concept for types that can allocate and deallocate memory for coroutine frames.

    Frame allocators are used to manage memory for coroutine frames, enabling
    custom allocation strategies such as pooling to reduce allocation overhead.

    Given:
    @li `a` a reference to type `A`
    @li `p` a void pointer
    @li `n` a size value (`std::size_t`)

    The following expressions must be valid:
    @li `a.allocate(n)` - Allocates `n` bytes and returns a pointer to the memory
    @li `a.deallocate(p, n)` - Deallocates `n` bytes previously allocated at `p`

    @tparam A The type to check for frame allocator conformance.
*/
template<class A>
concept frame_allocator = requires(A& a, void* p, std::size_t n) {
    { a.allocate(n) } -> std::convertible_to<void*>;
    { a.deallocate(p, n) } -> std::same_as<void>;
};

/** A concept for types that provide access to a frame allocator.

    Types satisfying this concept can be used as the first or second parameter
    to coroutine functions to enable custom frame allocation. The promise type
    will call `get_frame_allocator()` to obtain the allocator for the coroutine
    frame.

    Given:
    @li `t` a reference to type `T`

    The following expression must be valid:
    @li `t.get_frame_allocator()` - Returns a reference to a type satisfying
        `frame_allocator`

    @tparam T The type to check for frame allocator access.
*/
template<class T>
concept has_frame_allocator = requires(T& t) {
    { t.get_frame_allocator() } -> frame_allocator;
};

} // namespace capy

#endif
