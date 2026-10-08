//
// Copyright (c) 2025 Steve Gerbino (steve@gerbino.co)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_OBJECT_POOL_HPP
#define BOOST_COROSIO_DETAIL_OBJECT_POOL_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <utility>

namespace boost::corosio::detail {

/** Per-service recycling pool for I/O implementations.

    Replaces per-impl `shared_ptr` ownership: a service news an impl
    once, then `recycle()` (the impl's `retire()` action) parks
    it on the free list and `acquire()` hands it back out, so steady-state
    construct/destroy allocates nothing. Unbounded; memory is released
    only on pool destruction. In debug builds, recycled impls are
    poisoned by the service's `reuse()` discipline, not here (the pool
    cannot know which bytes are resettable).

    A service tears down its impls by calling `shutdown()`, which
    enters shutting-down mode and visits every live impl in one
    critical section; subsequent zero-ref crossings delete instead of
    recycling.

    `adopt()` and `remove()` are protected ownership-transfer
    primitives for sanctioned extensions — a derived pool exposing
    them to a caller that tracks some of its live impls through its
    own structure instead of this pool's `live_` list (the timer
    service, via its expiry heap) — rather than part of the public
    surface every ordinary service uses.

    @par Thread Safety
    Distinct objects: Safe. Shared objects: Safe; all operations lock
    an internal mutex.
*/
template<class Impl>
class object_pool
{
    std::mutex mutex_;
    intrusive_list<Impl> live_;
    intrusive_list<Impl> free_;
    bool shutting_down_ = false;

    /** Marks `refs_` on an impl currently parked in `free_`.

        `intrusive_list::remove()` cannot distinguish "linked in
        `free_`" from "linked in `live_`" by inspecting the node
        alone — both lists share one `next_`/`prev_` pair per impl —
        so `recycle()` cannot safely call `live_.remove()` on an impl
        it does not already know is live. Stamping this sentinel into
        `refs_` the moment an impl joins `free_`, and checking it
        before touching either list, is what lets `recycle()` and
        `remove()` tell the two cases apart in every build mode, not
        just under asserts. `acquire()` always overwrites it with 1
        before the impl is usable again.
    */
    static constexpr std::size_t pooled_sentinel =
        (std::numeric_limits<std::size_t>::max)();

    /// Check whether @p impl is parked in `free_` (`refs_` reads the
    /// sentinel). Callers must check this before any list operation —
    /// see the class-level comment on `pooled_sentinel`.
    static bool is_pooled(Impl const* impl) noexcept
    {
        return impl->refs_.load(std::memory_order_relaxed) ==
            pooled_sentinel;
    }

protected:
    /** Track a fresh or reset impl as live.

        @pre @p impl is not parked in `free_` (`refs_` must not read
        `pooled_sentinel`). The same list-corruption hazard
        `recycle()`'s class-level comment documents applies here: a
        free-listed impl pushed onto `live_` directly, bypassing
        `acquire()`, would end up linked in both lists through one shared
        `next_`/`prev_` pair.

        @pre The caller is not racing `shutdown()`. `acquire()` is
        this method's only caller, and a service's `construct()` —
        `acquire()`'s only caller — is expected to stop being called
        before the service's own `shutdown()` runs; this is checked
        only in debug builds because the failure mode is a silently
        orphaned impl (no close callback), not memory corruption.
    */
    void adopt(Impl* impl)
    {
        std::lock_guard lock(mutex_);
        if (is_pooled(impl))
        {
            BOOST_COROSIO_ASSERT(false);
            return;
        }
        BOOST_COROSIO_ASSERT(!shutting_down_);
        live_.push_back(impl);
    }

    /** Unlink a live impl without recycling (ownership transfer).

        Used by a caller that is taking an impl out of this pool's
        bookkeeping entirely (a thread-local cache slot, a direct
        delete during shutdown) rather than parking it on `free_`.

        @pre @p impl is not parked in `free_` (`refs_` must not read
        `pooled_sentinel`). The same list-corruption hazard
        `recycle()`'s class-level comment documents applies here: a
        free-listed impl would be spliced out using `live_`'s
        boundary pointers instead of `free_`'s.

        @return True if @p impl was linked in `live_` and has now
        been removed; false if it was already detached.
    */
    bool remove(Impl* impl) noexcept
    {
        std::lock_guard lock(mutex_);
        if (is_pooled(impl))
        {
            BOOST_COROSIO_ASSERT(false);
            return false;
        }
        return live_.remove(impl);
    }

public:
    /** Destroy the pool, freeing every impl still parked or live.

        @pre `shutdown()` has already run, or no impls are live.
        Without it, deleting a live impl here would let that impl's
        own destructor reenter `recycle()` outside the shutting-down
        branch that makes a reentrant retirement (a self-referential
        op's `object_ref` releasing its last reference while this
        destructor's own `delete` is still on the stack) a safe no-op
        — it would instead look exactly like the accounting-bug case
        `recycle()` deletes a second time.
    */
    ~object_pool()
    {
        BOOST_COROSIO_ASSERT(shutting_down_ || live_.empty());
        for (auto* list : {&free_, &live_})
            while (auto* impl = list->pop_front())
                delete impl;
    }

    object_pool() = default;
    object_pool(object_pool const&)            = delete;
    object_pool& operator=(object_pool const&) = delete;

    /** Pop-or-create an impl with the acquisition protocol applied.

        Recycled impls are `reuse()`-reset and their reference count
        restored from the pooled sentinel; fresh impls are constructed
        in place from `args` on the cold path. Either way the impl is
        adopted as live. This is the sequence every service's
        `construct()` used to hand-roll. Making the pool itself the
        only place that calls `new Impl` gives the whole library one
        audit point for allocation policy.

        @pre Not called concurrently with, or after, `shutdown()` on
        this pool. Every caller reaches this through a service's
        `construct()`, and service shutdown order guarantees no
        `construct()` outlives that service's own `shutdown()`; the
        window is only asserted in debug builds (see `adopt()`)
        because the failure mode is an orphaned impl, not corruption.

        @param args Constructor arguments for `Impl` (cold path only).

        @return The acquired impl, `refs_ == 1`, tracked live.
    */
    template<class... Args>
    Impl* acquire(Args&&... args)
    {
        Impl* impl;
        {
            std::lock_guard lock(mutex_);
            BOOST_COROSIO_ASSERT(!shutting_down_);
            impl = free_.pop_front();
            if (impl)
            {
                impl->refs_.store(1, std::memory_order_relaxed);
                live_.push_back(impl);
            }
        }
        // reuse() runs unlocked: acquire()'s precondition excludes a
        // concurrent shutdown() visiting the impl mid-reset.
        if (impl)
        {
            impl->reuse();
            return impl;
        }
        impl = new Impl(std::forward<Args>(args)...);
        adopt(impl);
        return impl;
    }

    /** Move a zero-ref impl from live to the free list.

        Called from `retire()`, possibly on a scheduler thread.
        Deletes instead when shutting down. The impl must not be
        touched after this returns.

        Two accounting bugs are possible here, neither from this
        pool's own bookkeeping but from a caller's refcounting (e.g.
        a duplicate `retire()` for one zero-crossing). Both are
        made safe in every build mode, not just under asserts, because
        neither can be told apart from the correct case by a
        `BOOST_COROSIO_ASSERT` alone that still runs the list
        operations around it:

        - `impl` is already parked in `free_` — the list-corruption
          hazard the class-level `pooled_sentinel` comment documents.
          The `is_pooled()` check runs first and returns before either
          list is touched.
        - `impl` is fully detached from `live_` and is not the
          sentinel (not shutting down): `live_.remove()` correctly
          returns false, but then the impl is reachable through
          neither `acquire()` nor `shutdown()`'s visit while still
          allocated — stranded. Deleted instead of left to leak.

        The one legitimate "already detached" case — an impl with an
        embedded op whose `object_ref` points back at itself reaching
        zero *during* `delete impl`, e.g. `~object_pool()`'s own
        unconditional sweep tearing down that op's `object_ref` member
        before the outer `delete` returns — is distinguished from the
        accounting-bug case by `shutting_down_`: that reentrant path
        only exists because `shutdown()` — called with a no-op
        callback by a caller like the timer service that tracks its
        live impls through its own structure instead of this pool's
        `live_` list, just to flip the flag — always precedes
        `~object_pool()`'s sweep (service shutdown order), so a
        not-live, not-pooled impl while `shutting_down_` is exactly
        that reentrant no-op, and the (already false) `was_live` means
        skip the delete — the outer frame's `delete` is still running.
    */
    void recycle(Impl* impl)
    {
        bool do_delete = false;
        {
            std::lock_guard lock(mutex_);
            if (is_pooled(impl))
            {
                BOOST_COROSIO_ASSERT(false);
                return;
            }

            bool const was_live = live_.remove(impl);
            if (shutting_down_)
            {
                do_delete = was_live;
            }
            else if (was_live)
            {
                impl->refs_.store(
                    pooled_sentinel, std::memory_order_relaxed);
                // LIFO: the most recently retired impl is the
                // cache-warmest candidate for the next acquire().
                free_.push_front(impl);
            }
            else
            {
                BOOST_COROSIO_ASSERT(false);
                do_delete = true;
            }
        }
        if (do_delete)
            delete impl;
    }

    /** Enter shutdown mode and visit every live impl.

        Sets shutting-down (subsequent zero-crossings delete instead
        of recycling) and runs `f` on each live impl, all under one
        critical section.

        @note The callback runs under the pool's non-recursive mutex;
        it must not call acquire(), recycle(), adopt(), or remove()
        on this pool.
    */
    template<class F>
    void shutdown(F&& f)
    {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        live_.for_each(std::forward<F>(f));
    }
};

} // namespace boost::corosio::detail

#endif
