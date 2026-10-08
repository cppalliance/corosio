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
#include <utility>

namespace boost::corosio::detail {

/// Add one reference to `impl`. `impl` must be non-null.
inline void
acquire(io_object::implementation* impl) noexcept
{
    impl->refs_.fetch_add(1, std::memory_order_relaxed);
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
    two words smaller than a shared_ptr and no control block.
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
