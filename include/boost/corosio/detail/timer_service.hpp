//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_TIMER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_TIMER_SERVICE_HPP

#include <boost/corosio/detail/timer.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <system_error>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

namespace boost::corosio::detail {

struct scheduler;

/*
    Timer Service
    =============

    Data Structures
    ---------------
    waiter_node (defined in timer.hpp) holds per-waiter state:
    coroutine handle, executor, error output, embedded
    completion_op. Each concurrent co_await t.wait() embeds one
    waiter_node in the awaitable on the suspended coroutine's
    frame — waits perform no allocation.

    timer::implementation holds per-timer state: expiry, heap
    index, and the single published waiter. Each timer holds
    at most one waiter; process_expired's local cross-timer drain
    list still threads waiters through their intrusive hooks when
    collecting several timers' waiters past the lock.

    timer_service owns a min-heap of active timers and a free list
    of recycled impls. The heap is ordered by expiry time; the
    scheduler queries nearest_expiry() to set the epoll/timerfd
    timeout.

    Optimization Strategy
    ---------------------
    1. Deferred heap insertion — expires_after() stores the expiry
       but does not insert into the heap. Insertion happens in wait().
    2. Thread-local impl cache — single-slot per-thread cache.
    3. Frame-resident waiter_node with embedded completion_op —
       eliminates heap allocation per wait/fire/cancel.
    4. Cached nearest expiry — atomic avoids mutex in nearest_expiry().
    5. might_have_pending_waits_ flag — skips lock when no wait issued.

    Concurrency
    -----------
    stop_token callbacks can fire from any thread. The impl_
    pointer on waiter_node is used as a "still in list" marker.
    A waiter_node's storage is the suspended coroutine's frame:
    every completion path must finish touching the node before
    posting the continuation or destroying the handle.
*/

inline void timer_service_invalidate_cache() noexcept;

// timer_service class body — member function definitions are
// out-of-class (after implementation and waiter_node are complete)
class BOOST_COROSIO_DECL timer_service final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;

    /// Type-erased callback for earliest-expiry-changed notifications.
    class callback
    {
        void* ctx_         = nullptr;
        void (*fn_)(void*) = nullptr;

    public:
        /// Construct an empty callback.
        callback() = default;

        /// Construct a callback with the given context and function.
        callback(void* ctx, void (*fn)(void*)) noexcept : ctx_(ctx), fn_(fn) {}

        /// Return true if the callback is non-empty.
        explicit operator bool() const noexcept
        {
            return fn_ != nullptr;
        }

        /// Invoke the callback.
        void operator()() const
        {
            if (fn_)
                fn_(ctx_);
        }
    };

private:
    struct heap_entry
    {
        time_point time_;
        timer::implementation* timer_;
    };

    scheduler* sched_ = nullptr;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // std:: members, dll-interface
    mutable std::mutex mutex_;
    std::vector<heap_entry> heap_;
    timer::implementation* free_list_ = nullptr;
    callback on_earliest_changed_;
    bool shutting_down_ = false;
    // Avoids mutex in nearest_expiry() and empty()
    mutable std::atomic<std::int64_t> cached_nearest_ns_{
        (std::numeric_limits<std::int64_t>::max)()};
    BOOST_COROSIO_MSVC_WARNING_POP

public:
    /// Construct the timer service bound to a scheduler.
    inline timer_service(capy::execution_context&, scheduler& sched)
        : sched_(&sched)
    {
    }

    /// Return the associated scheduler.
    inline scheduler& get_scheduler() noexcept
    {
        return *sched_;
    }

    /// Destroy the timer service.
    ~timer_service() override = default;

    timer_service(timer_service const&)            = delete;
    timer_service& operator=(timer_service const&) = delete;

    /// Register a callback invoked when the earliest expiry changes.
    inline void set_on_earliest_changed(callback cb)
    {
        on_earliest_changed_ = cb;
    }

    /// Return true if no timers are in the heap.
    inline bool empty() const noexcept
    {
        return cached_nearest_ns_.load(std::memory_order_acquire) ==
            (std::numeric_limits<std::int64_t>::max)();
    }

    /// Return the nearest timer expiry without acquiring the mutex.
    inline time_point nearest_expiry() const noexcept
    {
        auto ns = cached_nearest_ns_.load(std::memory_order_acquire);
        return time_point(time_point::duration(ns));
    }

    /// Cancel all pending timers and free cached resources.
    inline void shutdown() override;

    /// Construct a new timer implementation.
    inline io_object::implementation* construct() override;

    /// Destroy a timer implementation, cancelling pending waiters.
    inline void destroy(io_object::implementation* p) override;

    /// Cancel and recycle a timer implementation.
    inline void destroy_impl(timer::implementation& impl);

    /// Publish the timer's waiter and insert the timer into the heap.
    inline void insert_waiter(timer::implementation& impl, waiter_node* w);

    /// Cancel the timer's published waiter, if any.
    inline void cancel_timer(timer::implementation& impl);

    /// Cancel one specific waiter ( stop_token callback path ).
    inline void cancel_waiter(waiter_node* w);

    /// Complete all waiters whose timers have expired.
    inline std::size_t process_expired();

private:
    inline void refresh_cached_nearest() noexcept
    {
        auto ns = heap_.empty() ? (std::numeric_limits<std::int64_t>::max)()
                                : heap_[0].time_.time_since_epoch().count();
        cached_nearest_ns_.store(ns, std::memory_order_release);
    }

    inline void remove_timer_impl(timer::implementation& impl);
    inline void up_heap(std::size_t index);
    inline void down_heap(std::size_t index);
    inline void swap_heap(std::size_t i1, std::size_t i2);
};

// Thread-local cache avoids hot-path mutex acquisitions:
// single-slot impl cache, validated by comparing svc_. Cleared by
// timer_service_invalidate_cache() during shutdown.

inline thread_local_ptr<timer::implementation> tl_cached_impl;

// The POD TLS slot above never runs destructors, so a short-lived
// run() thread would leak its cached impl. Each push arms this
// owner, whose destructor frees the slot at thread exit. A cached
// entry is a quiescent heap object (nothing in the heap or free
// list) and deletion touches no service state, so it is safe after
// the owning service is gone (the stale-entry path in
// try_pop_tl_cache deletes the same way).
struct tl_cache_owner
{
    ~tl_cache_owner()
    {
        delete tl_cached_impl.get();
        tl_cached_impl.set(nullptr);
    }
};

inline void
arm_tl_cache_cleanup() noexcept
{
    [[maybe_unused]] thread_local tl_cache_owner owner;
}

inline timer::implementation*
try_pop_tl_cache(timer_service* svc) noexcept
{
    auto* impl = tl_cached_impl.get();
    if (impl)
    {
        tl_cached_impl.set(nullptr);
        if (impl->svc_ == svc)
            return impl;
        // Stale impl from a destroyed service
        delete impl;
    }
    return nullptr;
}

inline bool
try_push_tl_cache(timer::implementation* impl) noexcept
{
    if (!tl_cached_impl.get())
    {
        arm_tl_cache_cleanup();
        tl_cached_impl.set(impl);
        return true;
    }
    return false;
}

inline void
timer_service_invalidate_cache() noexcept
{
    delete tl_cached_impl.get();
    tl_cached_impl.set(nullptr);
}

// timer_service out-of-class member function definitions

inline void
timer_service::shutdown()
{
    timer_service_invalidate_cache();
    shutting_down_ = true;

    // Snapshot impls and detach them from the heap so that
    // coroutine-owned timer destructors (triggered by h.destroy()
    // below) cannot re-enter remove_timer_impl() and mutate the
    // vector during iteration.
    std::vector<timer::implementation*> impls;
    impls.reserve(heap_.size());
    for (auto& entry : heap_)
    {
        entry.timer_->heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        impls.push_back(entry.timer_);
    }
    heap_.clear();
    cached_nearest_ns_.store(
        (std::numeric_limits<std::int64_t>::max)(), std::memory_order_release);

    // Cancel waiting timers. Each waiter called work_started()
    // in implementation::wait(). On IOCP the scheduler shutdown
    // loop exits when outstanding_work_ reaches zero, so we must
    // call work_finished() here to balance it. On other backends
    // this is harmless.
    for (auto* impl : impls)
    {
        if (auto* w = std::exchange(impl->waiter_, nullptr))
        {
            w->reset_stop_cb();
            auto h = std::exchange(w->h_, {});
            sched_->work_finished();
            // Destroying the frame also ends the node's storage
            if (h)
                h.destroy();
        }
        delete impl;
    }

    // Delete free-listed impls
    while (free_list_)
    {
        auto* next = free_list_->next_free_;
        delete free_list_;
        free_list_ = next;
    }
}

inline io_object::implementation*
timer_service::construct()
{
    timer::implementation* impl = try_pop_tl_cache(this);
    if (impl)
    {
        impl->svc_    = this;
        // Reset expiry_ too: a recycled impl must behave like a fresh
        // one, whose default expiry reads as already elapsed
        impl->expiry_ = {};
        impl->heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        impl->might_have_pending_waits_.store(false, std::memory_order_relaxed);
        BOOST_COROSIO_ASSERT(impl->waiter_ == nullptr);
        return impl;
    }

    std::lock_guard lock(mutex_);
    if (free_list_)
    {
        impl              = free_list_;
        free_list_        = impl->next_free_;
        impl->next_free_  = nullptr;
        impl->svc_        = this;
        impl->expiry_     = {};
        impl->heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        impl->might_have_pending_waits_.store(false, std::memory_order_relaxed);
        BOOST_COROSIO_ASSERT(impl->waiter_ == nullptr);
    }
    else
    {
        impl = new timer::implementation(*this);
    }
    return impl;
}

inline void
timer_service::destroy(io_object::implementation* p)
{
    // During shutdown the drain loop owns every impl and deletes
    // them directly. A frame destroyed by that loop can unwind a
    // handle whose impl was freed in an earlier iteration (a
    // timeout's parent frame owns the timeout timer while
    // suspended on the inner delay's timer), so bail out before
    // even downcasting the pointer.
    if (shutting_down_)
        return;
    destroy_impl(static_cast<timer::implementation&>(*p));
}

inline void
timer_service::destroy_impl(timer::implementation& impl)
{
    // During shutdown the impl is owned by the shutdown loop.
    // Re-entering here (from a coroutine-owned timer destructor
    // triggered by h.destroy()) must not modify the heap or
    // recycle the impl — shutdown deletes it directly.
    if (shutting_down_)
        return;

    cancel_timer(impl);

    if (impl.heap_index_.load(std::memory_order_relaxed) !=
        (std::numeric_limits<std::size_t>::max)())
    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
        refresh_cached_nearest();
    }

    if (try_push_tl_cache(&impl))
        return;

    std::lock_guard lock(mutex_);
    impl.next_free_ = free_list_;
    free_list_      = &impl;
}

inline void
timer_service::insert_waiter(timer::implementation& impl, waiter_node* w)
{
    bool notify      = false;
    bool lost_cancel = false;
    {
        std::lock_guard lock(mutex_);
        // Grow before publishing anything, so the push_back below
        // cannot throw: a failure here leaves the waiter untouched,
        // the strong guarantee rearm_wait's recovery relies on.
        if (impl.heap_index_.load(std::memory_order_relaxed) ==
                (std::numeric_limits<std::size_t>::max)() &&
            heap_.size() == heap_.capacity())
            heap_.reserve(
                heap_.capacity() == 0 ? 16 : 2 * heap_.capacity());
        // Publish: from here the waiter is visible to the fire path and
        // to its own stop callback (impl_ non-null enables cancel_waiter).
        w->impl_ = &impl;
        if (impl.heap_index_.load(std::memory_order_relaxed) ==
            (std::numeric_limits<std::size_t>::max)())
        {
            impl.heap_index_.store(heap_.size(), std::memory_order_relaxed);
            heap_.push_back({impl.expiry_, &impl});
            up_heap(heap_.size() - 1);
            notify =
                (impl.heap_index_.load(std::memory_order_relaxed) == 0);
            refresh_cached_nearest();
        }
        BOOST_COROSIO_ASSERT(impl.waiter_ == nullptr);
        impl.waiter_ = w;

        // Lost-cancel re-check: a stop requested after the canceller was
        // armed in wait() but before this publication found impl_ null
        // and returned a no-op. Observe it now and undo the insertion.
        if (w->token_->stop_requested())
        {
            w->impl_     = nullptr;
            impl.waiter_ = nullptr;
            remove_timer_impl(impl);
            impl.might_have_pending_waits_.store(
                false, std::memory_order_relaxed);
            refresh_cached_nearest();
            lost_cancel = true;
            notify      = false; // insertion undone; nearest unchanged
        }
    }
    if (notify)
        on_earliest_changed_();
    if (lost_cancel)
    {
        w->ec_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
    }
}

inline void
timer_service::cancel_timer(timer::implementation& impl)
{
    if (!impl.might_have_pending_waits_.load(std::memory_order_relaxed))
        return;

    // No unlocked already-done fast-out here: it would need the
    // non-atomic waiter_ (a race with concurrent drains), and an
    // index-only check is lifetime-unsafe because npos is stored
    // before the drain finishes touching the impl. A stale-true
    // flag is rare with the stateless API; the locked path below
    // re-validates.

    waiter_node* canceled = nullptr;

    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
        canceled = std::exchange(impl.waiter_, nullptr);
        if (canceled)
            canceled->impl_ = nullptr;
        // Store false as the final touch of the impl under the lock so
        // a pre-lock false-flag check trusts it unqualified.
        impl.might_have_pending_waits_.store(false, std::memory_order_relaxed);
        refresh_cached_nearest();
    }

    if (canceled)
    {
        canceled->ec_ = make_error_code(capy::error::canceled);
        sched_->post(&canceled->op_);
    }
}

inline void
timer_service::cancel_waiter(waiter_node* w)
{
    {
        std::lock_guard lock(mutex_);
        // Already removed by another drain: cancel_timer,
        // process_expired, or insert_waiter's lost-cancel recheck
        if (!w->impl_)
            return;
        auto* impl    = w->impl_;
        w->impl_      = nullptr;
        impl->waiter_ = nullptr;
        remove_timer_impl(*impl);
        impl->might_have_pending_waits_.store(
            false, std::memory_order_relaxed);
        refresh_cached_nearest();
    }

    w->ec_ = make_error_code(capy::error::canceled);
    sched_->post(&w->op_);
}

inline std::size_t
timer_service::process_expired()
{
    intrusive_list<waiter_node> expired;

    {
        std::lock_guard lock(mutex_);
        auto now = clock_type::now();

        while (!heap_.empty() && heap_[0].time_ <= now)
        {
            timer::implementation* t = heap_[0].timer_;
            remove_timer_impl(*t);
            if (auto* w = std::exchange(t->waiter_, nullptr))
            {
                w->impl_ = nullptr;
                w->ec_   = {};
                expired.push_back(w);
            }
            t->might_have_pending_waits_.store(
                false, std::memory_order_relaxed);
        }

        refresh_cached_nearest();
    }

    std::size_t count = 0;
    while (auto* w = expired.pop_front())
    {
        sched_->post(&w->op_);
        ++count;
    }

    return count;
}

inline void
timer_service::remove_timer_impl(timer::implementation& impl)
{
    std::size_t index = impl.heap_index_.load(std::memory_order_relaxed);
    if (index >= heap_.size())
        return; // Not in heap

    if (index == heap_.size() - 1)
    {
        // Last element, just pop
        impl.heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        heap_.pop_back();
    }
    else
    {
        // Swap with last and reheapify
        swap_heap(index, heap_.size() - 1);
        impl.heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        heap_.pop_back();

        if (index > 0 && heap_[index].time_ < heap_[(index - 1) / 2].time_)
            up_heap(index);
        else
            down_heap(index);
    }
}

inline void
timer_service::up_heap(std::size_t index)
{
    while (index > 0)
    {
        std::size_t parent = (index - 1) / 2;
        if (!(heap_[index].time_ < heap_[parent].time_))
            break;
        swap_heap(index, parent);
        index = parent;
    }
}

inline void
timer_service::down_heap(std::size_t index)
{
    std::size_t child = index * 2 + 1;
    while (child < heap_.size())
    {
        std::size_t min_child = (child + 1 == heap_.size() ||
                                 heap_[child].time_ < heap_[child + 1].time_)
            ? child
            : child + 1;

        if (heap_[index].time_ < heap_[min_child].time_)
            break;

        swap_heap(index, min_child);
        index = min_child;
        child = index * 2 + 1;
    }
}

inline void
timer_service::swap_heap(std::size_t i1, std::size_t i2)
{
    heap_entry tmp                = heap_[i1];
    heap_[i1]                     = heap_[i2];
    heap_[i2]                     = tmp;
    heap_[i1].timer_->heap_index_.store(i1, std::memory_order_relaxed);
    heap_[i2].timer_->heap_index_.store(i2, std::memory_order_relaxed);
}

// waiter_node's completion_op and canceller members are defined in
// timer.cpp alongside implementation::wait(), for the same reason
// wait() lives there (see below).

// timer::implementation::wait() is defined in timer.cpp, not here.
// It must be a non-inline definition in a translation unit that is
// always pulled into the link whenever detail::timer is used (every
// consumer needs timer's constructors from that same object file).
// An inline definition in this header would only be emitted in
// translation units that happen to also include this header, which
// is not guaranteed for every caller of wait_awaitable::await_suspend
// in timer.hpp (e.g. code that only reaches timer.hpp through
// delay.hpp, without transitively including a scheduler header).

// Free functions

inline timer_service&
get_timer_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<timer_service>(sched);
}

} // namespace boost::corosio::detail

#endif
