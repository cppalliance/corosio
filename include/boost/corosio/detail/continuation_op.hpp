//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CONTINUATION_OP_HPP
#define BOOST_COROSIO_DETAIL_CONTINUATION_OP_HPP

#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/continuation.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace boost::corosio::detail {

/* Scheduler operation that resumes a capy::continuation.

   Embeds a continuation alongside a scheduler_op so the
   scheduler can queue it in the same FIFO as I/O completions
   without a heap allocation. The continuation lives in the
   caller's coroutine frame (awaitable or op struct); this
   wrapper gives it a scheduler_op identity.

   io_context::executor_type::post(continuation&) uses
   try_from_continuation() to recover the enclosing
   continuation_op via a magic tag. The tag is read through
   memcpy (not through a continuation_op*) so that UBSan
   does not flag the speculative pointer arithmetic when the
   continuation is not actually inside a continuation_op.
*/
struct continuation_op final : scheduler_op
{
    static constexpr std::uint32_t magic_ = 0xC0710Au;

    std::uint32_t tag_ = magic_;
    capy::continuation cont;

    continuation_op() noexcept : scheduler_op(&do_complete) {}

    // Reactor backends (epoll, select, kqueue) dispatch through
    // virtual operator()(). IOCP dispatches through func_ which
    // routes to do_complete below.
    void operator()() override
    {
        // ThreadSanitizer cannot instrument standalone fences; this acquire
        // fence pairs with the scheduler's release and is intentional.
        BOOST_COROSIO_GCC_WARNING_PUSH
        BOOST_COROSIO_GCC_WARNING_DISABLE("-Wtsan")
        std::atomic_thread_fence(std::memory_order_acquire);
        BOOST_COROSIO_GCC_WARNING_POP
        cont.h.resume();
    }

    void destroy() override
    {
        if (cont.h)
            cont.h.destroy();
    }

private:
    // IOCP completion entry point. owner == nullptr means destroy.
    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t,
        std::uint32_t)
    {
        auto* self = static_cast<continuation_op*>(base);
        if (!owner)
        {
            if (self->cont.h)
                self->cont.h.destroy();
            return;
        }
        BOOST_COROSIO_GCC_WARNING_PUSH
        BOOST_COROSIO_GCC_WARNING_DISABLE("-Wtsan")
        std::atomic_thread_fence(std::memory_order_acquire);
        BOOST_COROSIO_GCC_WARNING_POP
        self->cont.h.resume();
    }

public:

    // Recover the enclosing continuation_op from its cont member.
    // Returns nullptr if the continuation is not tagged (bare
    // capy::continuation from capy internals like run_async).
    static continuation_op* try_from_continuation(
        capy::continuation& c) noexcept
    {
        // offsetof on non-standard-layout is conditionally-supported;
        // suppress the warning — all targeted compilers handle this
        // correctly and the self-relative arithmetic is move-safe.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
        constexpr auto cont_off = offsetof(continuation_op, cont);
        constexpr auto tag_off  = offsetof(continuation_op, tag_);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        // Read the tag through memcpy from a char*, not through a
        // continuation_op*. This avoids UBSan's vptr check when
        // the continuation is not actually inside a continuation_op.
        auto* base = reinterpret_cast<char*>(&c) - cont_off;
        std::uint32_t tag;
        std::memcpy(&tag, base + tag_off, sizeof(tag));
        if (tag != magic_)
            return nullptr;
        return reinterpret_cast<continuation_op*>(base);
    }
};

} // namespace boost::corosio::detail

#endif
