//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_OBJECT_REF_HPP
#define BOOST_COROSIO_DETAIL_OBJECT_REF_HPP

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <atomic>
#include <cstddef>
#include <limits>
#include <utility>

namespace boost::corosio::detail {

/// Add one reference to `impl`. `impl` must be non-null.
inline void
acquire(io_object::implementation* impl) noexcept
{
    impl->refs_.fetch_add(1, std::memory_order_relaxed);
}

/** The count an `object_pool` stamps on an impl parked for reuse.

    It holds no reference; see `object_pool`.
*/
inline constexpr std::size_t pooled_refs =
    (std::numeric_limits<std::size_t>::max)();

/** Add one reference to `impl` unless it is retiring or parked.

    A count at zero means `retire()` is running or about to, and a
    count of `pooled_refs` means the impl waits in its pool for a new
    owner; neither may be revived.

    @return True if a reference was added.
*/
inline bool
try_acquire(io_object::implementation* impl) noexcept
{
    auto n = impl->refs_.load(std::memory_order_relaxed);
    while (n != 0 && n != pooled_refs)
    {
        if (impl->refs_.compare_exchange_weak(
                n, n + 1, std::memory_order_relaxed))
            return true;
    }
    return false;
}

/** Drop one reference; runs `retire()` at zero. `impl` must be
    non-null.

    Uses release on the decrement and an acquire fence before the
    zero-action so writes made while holding a reference are visible
    to the recycler. TSan cannot model a standalone fence, so under
    it the decrement itself is acq_rel instead -- strictly stronger,
    and TSan's happens-before tracking models the exchange directly.
*/
inline void
release(io_object::implementation* impl) noexcept
{
#if BOOST_COROSIO_TSAN
    if (impl->refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        impl->retire();
#else
    if (impl->refs_.fetch_sub(1, std::memory_order_release) == 1)
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        impl->retire();
    }
#endif
}

/** Owning intrusive reference to an I/O implementation.

    Replaces `std::shared_ptr<void>` op keepalives: copy acquires,
    move steals, destruction releases. Kept deliberately minimal —
    one pointer, half the size of a shared_ptr, and no control block.
*/
class object_ref
{
    io_object::implementation* p_ = nullptr;

public:
    /// Destroy the object_ref, releasing the held reference, if any.
    ~object_ref() { reset(); }

    /// Construct an object_ref holding no reference.
    object_ref() noexcept = default;

    /// Construct an object_ref that acquires a new reference to `p`.
    explicit object_ref(io_object::implementation* p) noexcept : p_(p)
    {
        if (p_)
            acquire(p_);
    }

    /** Create a reference to `p` unless its count already reached zero.

        The way to take a reference from a raw pointer: a context that
        holds none may find the count at zero, and must not revive it.

        @param p The implementation to reference.

        @return A reference to `p`, or an empty `object_ref` if `p` is
            retiring.
    */
    static object_ref try_from(io_object::implementation* p) noexcept
    {
        object_ref r;
        if (try_acquire(p))
            r.p_ = p;
        return r;
    }

    /// Construct a copy, acquiring another reference to `other`'s target.
    object_ref(object_ref const& other) noexcept : p_(other.p_)
    {
        if (p_)
            acquire(p_);
    }

    /// Construct by transferring the reference out of `other`.
    object_ref(object_ref&& other) noexcept
        : p_(std::exchange(other.p_, nullptr))
    {
    }

    /// Release the held reference, if any, and acquire `other`'s target.
    object_ref& operator=(object_ref const& other) noexcept
    {
        object_ref(other).swap(*this);
        return *this;
    }

    /// Release the held reference, if any, and take ownership of `other`'s.
    object_ref& operator=(object_ref&& other) noexcept
    {
        object_ref(std::move(other)).swap(*this);
        return *this;
    }

    /// Release the held reference, if any.
    void reset() noexcept
    {
        if (p_)
            release(std::exchange(p_, nullptr));
    }

    /// Return the held implementation pointer.
    io_object::implementation* get() const noexcept { return p_; }

    /// Return true if a reference is held.
    explicit operator bool() const noexcept { return p_ != nullptr; }

    /// Exchange the implementation pointers held by `*this` and `other`.
    void swap(object_ref& other) noexcept { std::swap(p_, other.p_); }
};

} // namespace boost::corosio::detail

#endif
