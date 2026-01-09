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

/** A type-erased frame allocator.

    This class wraps any type satisfying the `frame_allocator` concept,
    storing only a pointer to the allocator and a pointer to a static
    operations table. This enables polymorphic frame allocation without
    virtual functions or heap allocation for the wrapper itself.

    The wrapped allocator must outlive the `any_frame_allocator` instance.

    @see frame_allocator
*/
class any_frame_allocator
{
    struct ops
    {
        void* (*allocate)(void* alloc, std::size_t n);
        void (*deallocate)(void* alloc, void* p, std::size_t n);
    };

    template<frame_allocator A>
    static ops const&
    ops_for() noexcept
    {
        static constexpr ops o = {
            [](void* alloc, std::size_t n) -> void* {
                return static_cast<A*>(alloc)->allocate(n);
            },
            [](void* alloc, void* p, std::size_t n) {
                static_cast<A*>(alloc)->deallocate(p, n);
            }
        };
        return o;
    }

    void* alloc_;
    ops const* ops_;

public:
    /** Construct from a frame allocator.

        @param alloc The allocator to wrap. Must outlive this instance.
    */
    template<frame_allocator A>
    any_frame_allocator(A& alloc) noexcept
        : alloc_(&alloc)
        , ops_(&ops_for<A>())
    {
    }

    /** Allocate memory for a coroutine frame.

        @param n The number of bytes to allocate.

        @return A pointer to the allocated memory.
    */
    void*
    allocate(std::size_t n)
    {
        return ops_->allocate(alloc_, n);
    }

    /** Deallocate memory for a coroutine frame.

        @param p Pointer to the memory to deallocate.
        @param n The number of bytes to deallocate.
    */
    void
    deallocate(void* p, std::size_t n)
    {
        ops_->deallocate(alloc_, p, n);
    }
};

static_assert(frame_allocator<any_frame_allocator>);

} // namespace capy

#endif
