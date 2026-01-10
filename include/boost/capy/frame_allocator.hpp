//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_CAPY_FRAME_ALLOCATOR_HPP
#define BOOST_CAPY_FRAME_ALLOCATOR_HPP

#include <boost/capy/config.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>

namespace boost {
namespace capy {

//----------------------------------------------------------
// Public API
//----------------------------------------------------------

/** A concept for types that can allocate and deallocate coroutine frames.

    Frame allocators must be cheaply copyable handles to an underlying
    memory resource (e.g., a pointer to a pool). The framework copies
    the allocator into the first coroutine frame for lifetime safety.

    @par Requirements

    Given:
    @li `a` a reference to type `A`
    @li `p` a `void*`
    @li `n` a `std::size_t`

    The following expressions must be valid:
    @li `a.allocate(n)` - Returns `void*`
    @li `a.deallocate(p, n)` - Returns void

    @tparam A The type to check for frame allocator conformance.
*/
template<class A>
concept frame_allocator =
    std::copy_constructible<A> &&
    requires(A& a, void* p, std::size_t n) {
        { a.allocate(n) } -> std::same_as<void*>;
        { a.deallocate(p, n) };
    };

/** A frame allocator that passes through to global new/delete.

    This allocator provides no pooling or recycling—each allocation
    goes directly to `::operator new` and each deallocation goes to
    `::operator delete`. It serves as a baseline for comparison and
    as a fallback when pooling is not desired.
*/
struct default_frame_allocator
{
    void* allocate(std::size_t n)
    {
        return ::operator new(n);
    }

    void deallocate(void* p, std::size_t)
    {
        ::operator delete(p);
    }
};

static_assert(frame_allocator<default_frame_allocator>);

//----------------------------------------------------------
// Implementation details
//----------------------------------------------------------

namespace detail {

/** Abstract base class for internal frame allocator wrappers.

    This class provides a polymorphic interface used internally
    by the frame allocation machinery. User-defined allocators
    do not inherit from this class.
*/
class frame_allocator_base
{
public:
    virtual ~frame_allocator_base() = default;

    /** Allocate memory for a coroutine frame.

        @param n The number of bytes to allocate.

        @return A pointer to the allocated memory.
    */
    virtual void* allocate(std::size_t n) = 0;

    /** Deallocate memory for a child coroutine frame.

        @param p Pointer to the memory to deallocate.
        @param n The user-requested size (not total allocation).
    */
    virtual void deallocate(void* p, std::size_t n) = 0;

    /** Deallocate the first coroutine frame (where this wrapper is embedded).

        This method handles the special case where the wrapper itself
        is embedded at the end of the block being deallocated.

        @param block Pointer to the block to deallocate.
        @param user_size The user-requested size (not total allocation).
    */
    virtual void deallocate_embedded(void* block, std::size_t user_size) = 0;
};

// Forward declaration
template<frame_allocator Allocator>
class frame_allocator_wrapper;

/** Wrapper that embeds a frame_allocator_wrapper in the first allocation.

    This wrapper lives on the stack (in async_run_awaitable) and is used only
    for the FIRST coroutine frame allocation. It embeds a copy of
    frame_allocator_wrapper at the end of the allocated block, then
    updates TLS to point to that embedded wrapper for subsequent
    allocations.

    @tparam Allocator The underlying allocator type satisfying frame_allocator.
*/
template<frame_allocator Allocator>
class embedding_frame_allocator : public frame_allocator_base
{
    Allocator alloc_;

    static constexpr std::size_t alignment = alignof(void*);

    static_assert(
        alignof(frame_allocator_wrapper<Allocator>) <= alignment,
        "alignment must be at least as strict as wrapper alignment");

    static std::size_t
    aligned_offset(std::size_t n) noexcept
    {
        return (n + alignment - 1) & ~(alignment - 1);
    }

public:
    explicit embedding_frame_allocator(Allocator a)
        : alloc_(std::move(a))
    {
    }

    void*
    allocate(std::size_t n) override;

    void
    deallocate(void*, std::size_t) override
    {
        // Never called - stack wrapper not used for deallocation
    }

    void
    deallocate_embedded(void*, std::size_t) override
    {
        // Never called
    }
};

/** Wrapper embedded in the first coroutine frame.

    This wrapper is constructed at the end of the first coroutine
    frame by embedding_frame_allocator. It handles all subsequent
    allocations (storing a pointer to itself) and all deallocations.

    @tparam Allocator The underlying allocator type satisfying frame_allocator.
*/
template<frame_allocator Allocator>
class frame_allocator_wrapper : public frame_allocator_base
{
    Allocator alloc_;

    static constexpr std::size_t alignment = alignof(void*);

    static std::size_t
    aligned_offset(std::size_t n) noexcept
    {
        return (n + alignment - 1) & ~(alignment - 1);
    }

public:
    explicit frame_allocator_wrapper(Allocator a)
        : alloc_(std::move(a))
    {
    }

    void*
    allocate(std::size_t n) override
    {
        // Layout: [frame | ptr]
        std::size_t ptr_offset = aligned_offset(n);
        std::size_t total = ptr_offset + sizeof(frame_allocator_base*);

        void* raw = alloc_.allocate(total);

        // Store untagged pointer to self at fixed offset
        auto* ptr_loc = reinterpret_cast<frame_allocator_base**>(
            static_cast<char*>(raw) + ptr_offset);
        *ptr_loc = this;

        return raw;
    }

    void
    deallocate(void* block, std::size_t user_size) override
    {
        // Child frame deallocation: layout is [frame | ptr]
        std::size_t ptr_offset = aligned_offset(user_size);
        std::size_t total = ptr_offset + sizeof(frame_allocator_base*);
        alloc_.deallocate(block, total);
    }

    void
    deallocate_embedded(void* block, std::size_t user_size) override
    {
        // First frame deallocation: layout is [frame | ptr | wrapper]
        std::size_t ptr_offset = aligned_offset(user_size);
        std::size_t wrapper_offset = ptr_offset + sizeof(frame_allocator_base*);
        std::size_t total = wrapper_offset + sizeof(frame_allocator_wrapper);

        Allocator alloc_copy = alloc_;  // Copy before destroying self
        this->~frame_allocator_wrapper();
        alloc_copy.deallocate(block, total);
    }
};

} // namespace detail

/** Mixin base for promise types to support custom frame allocation.

    Derive your promise_type from this class to enable custom coroutine
    frame allocation via a thread-local allocator pointer.

    The allocation strategy:
    @li If a thread-local allocator is set, use it for allocation
    @li Otherwise, fall back to global `::operator new`/`::operator delete`

    A pointer is stored at the end of each allocation to enable correct
    deallocation regardless of which allocator was active at allocation time.

    @par Memory Layout

    For the first coroutine frame (allocated via embedding_frame_allocator):
    @code
    [coroutine frame | tagged_ptr | frame_allocator_wrapper]
    @endcode

    For subsequent frames (allocated via frame_allocator_wrapper):
    @code
    [coroutine frame | ptr]
    @endcode

    The tag bit (low bit) distinguishes the two cases during deallocation.

    @see frame_allocator
*/
struct frame_allocating_base
{
private:
    static constexpr std::size_t alignment = alignof(void*);

    static std::size_t
    aligned_offset(std::size_t n) noexcept
    {
        return (n + alignment - 1) & ~(alignment - 1);
    }

    static detail::frame_allocator_base*&
    current_allocator() noexcept
    {
        static thread_local detail::frame_allocator_base* alloc = nullptr;
        return alloc;
    }

public:
    /** Set the thread-local frame allocator.

        The allocator will be used for subsequent coroutine frame
        allocations on this thread until changed or cleared.

        @param alloc The allocator to use. Must outlive all coroutines
                     allocated with it.
    */
    static void
    set_frame_allocator(detail::frame_allocator_base& alloc) noexcept
    {
        current_allocator() = &alloc;
    }

    /** Clear the thread-local frame allocator.

        Subsequent allocations will use global `::operator new`.
    */
    static void
    clear_frame_allocator() noexcept
    {
        current_allocator() = nullptr;
    }

    /** Get the current thread-local frame allocator.

        @return Pointer to current allocator, or nullptr if none set.
    */
    static detail::frame_allocator_base*
    get_frame_allocator() noexcept
    {
        return current_allocator();
    }

    static void*
    operator new(std::size_t size)
    {
        auto* alloc = current_allocator();
        if(!alloc)
        {
            // No allocator: allocate extra space for null pointer marker
            std::size_t ptr_offset = aligned_offset(size);
            std::size_t total = ptr_offset + sizeof(detail::frame_allocator_base*);
            void* raw = ::operator new(total);

            // Store nullptr to indicate global new/delete
            auto* ptr_loc = reinterpret_cast<detail::frame_allocator_base**>(
                static_cast<char*>(raw) + ptr_offset);
            *ptr_loc = nullptr;

            return raw;
        }
        return alloc->allocate(size);
    }

    /** Deallocate a coroutine frame.

        Reads the pointer stored at the end of the frame to find
        the allocator. The tag bit (low bit) indicates whether
        this is the first frame (with embedded wrapper) or a
        child frame (with pointer to external wrapper).

        A null pointer indicates the frame was allocated with
        global new/delete (no custom allocator was active).
    */
    static void
    operator delete(void* ptr, std::size_t size)
    {
        // Pointer is always at aligned_offset(size)
        std::size_t ptr_offset = aligned_offset(size);
        auto* ptr_loc = reinterpret_cast<detail::frame_allocator_base**>(
            static_cast<char*>(ptr) + ptr_offset);
        auto raw_ptr = reinterpret_cast<std::uintptr_t>(*ptr_loc);

        // Null pointer means global new/delete
        if(raw_ptr == 0)
        {
            std::size_t total = ptr_offset + sizeof(detail::frame_allocator_base*);
            ::operator delete(ptr, total);
            return;
        }

        // Tag bit distinguishes first frame (embedded) from child frames
        bool is_embedded = raw_ptr & 1;
        auto* wrapper = reinterpret_cast<detail::frame_allocator_base*>(
            raw_ptr & ~std::uintptr_t(1));

        if(is_embedded)
            wrapper->deallocate_embedded(ptr, size);
        else
            wrapper->deallocate(ptr, size);
    }
};

//----------------------------------------------------------
// embedding_frame_allocator implementation
// (must come after frame_allocating_base is defined)
//----------------------------------------------------------

namespace detail {

template<frame_allocator Allocator>
void*
embedding_frame_allocator<Allocator>::allocate(std::size_t n)
{
    // Layout: [frame | ptr | wrapper]
    std::size_t ptr_offset = aligned_offset(n);
    std::size_t wrapper_offset = ptr_offset + sizeof(frame_allocator_base*);
    std::size_t total = wrapper_offset + sizeof(frame_allocator_wrapper<Allocator>);

    void* raw = alloc_.allocate(total);

    // Construct embedded wrapper after the pointer
    auto* wrapper_loc = static_cast<char*>(raw) + wrapper_offset;
    auto* embedded = new (wrapper_loc) frame_allocator_wrapper<Allocator>(alloc_);

    // Store tagged pointer at fixed offset (bit 0 set = embedded)
    auto* ptr_loc = reinterpret_cast<frame_allocator_base**>(
        static_cast<char*>(raw) + ptr_offset);
    *ptr_loc = reinterpret_cast<frame_allocator_base*>(
        reinterpret_cast<std::uintptr_t>(embedded) | 1);

    // Update TLS to embedded wrapper for subsequent allocations
    frame_allocating_base::set_frame_allocator(*embedded);

    return raw;
}

} // namespace detail

} // namespace capy
} // namespace boost

#endif
