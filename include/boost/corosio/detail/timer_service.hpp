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
#include <optional>
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
    waiter_node holds per-waiter state: coroutine handle, executor,
    error output, stop_token, embedded completion_op. Each concurrent
    co_await t.wait() allocates one waiter_node.

    timer::implementation holds per-timer state: expiry, heap
    index, and an intrusive_list of waiter_nodes. Multiple
    coroutines can wait on the same timer simultaneously.

    timer_service owns a min-heap of active timers, a free list
    of recycled impls, and a free list of recycled waiter_nodes. The
    heap is ordered by expiry time; the scheduler queries
    nearest_expiry() to set the epoll/timerfd timeout.

    Optimization Strategy
    ---------------------
    1. Deferred heap insertion — expires_after() stores the expiry
       but does not insert into the heap. Insertion happens in wait().
    2. Thread-local impl cache — single-slot per-thread cache.
    3. Embedded completion_op — eliminates heap allocation per fire/cancel.
    4. Cached nearest expiry — atomic avoids mutex in nearest_expiry().
    5. might_have_pending_waits_ flag — skips lock when no wait issued.
    6. Thread-local waiter cache — single-slot per-thread cache.

    Concurrency
    -----------
    stop_token callbacks can fire from any thread. The impl_
    pointer on waiter_node is used as a "still in list" marker.
*/

struct BOOST_COROSIO_SYMBOL_VISIBLE waiter_node;

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
    waiter_node* waiter_free_list_ = nullptr;
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

    /// Create or recycle a waiter node.
    inline waiter_node* create_waiter();

    /// Return a waiter node to the cache or free list.
    inline void destroy_waiter(waiter_node* w);

    /// Update the timer expiry, cancelling existing waiters.
    inline std::size_t update_timer(
        timer::implementation& impl, time_point new_time);

    /// Insert a waiter into the timer's waiter list and the heap.
    inline void insert_waiter(timer::implementation& impl, waiter_node* w);

    /// Cancel all waiters on a timer.
    inline std::size_t cancel_timer(timer::implementation& impl);

    /// Cancel a single waiter ( stop_token callback path ).
    inline void cancel_waiter(waiter_node* w);

    /// Cancel one waiter on a timer.
    inline std::size_t cancel_one_waiter(timer::implementation& impl);

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

struct BOOST_COROSIO_SYMBOL_VISIBLE waiter_node
    : intrusive_list<waiter_node>::node
{
    // Embedded completion op — avoids heap allocation per fire/cancel
    struct completion_op final : scheduler_op
    {
        waiter_node* waiter_ = nullptr;

        static void do_complete(
            void* owner, scheduler_op* base, std::uint32_t, std::uint32_t);

        completion_op() noexcept : scheduler_op(&do_complete) {}

        void operator()() override;
        void destroy() override;
    };

    // Per-waiter stop_token cancellation
    struct canceller
    {
        waiter_node* waiter_;
        void operator()() const;
    };

    // nullptr once removed from timer's waiter list (concurrency marker)
    timer::implementation* impl_ = nullptr;
    timer_service* svc_                  = nullptr;
    std::coroutine_handle<> h_;
    capy::continuation* cont_            = nullptr;
    capy::executor_ref d_;
    std::error_code* ec_out_ = nullptr;
    std::stop_token token_;
    std::optional<std::stop_callback<canceller>> stop_cb_;
    completion_op op_;
    std::error_code ec_value_;
    waiter_node* next_free_ = nullptr;

    waiter_node() noexcept
    {
        op_.waiter_ = this;
    }
};

// Thread-local caches avoid hot-path mutex acquisitions:
// 1. Impl cache — single-slot, validated by comparing svc_
// 2. Waiter cache — single-slot, no service affinity
// All caches are cleared by timer_service_invalidate_cache() during shutdown.

inline thread_local_ptr<timer::implementation> tl_cached_impl;
inline thread_local_ptr<waiter_node> tl_cached_waiter;

// The POD TLS slots above never run destructors, so a short-lived
// run() thread would leak its cached impl and waiter. Each push
// arms this owner, whose destructor frees the slots at thread exit.
// Cached entries are quiescent heap objects (stop callbacks reset,
// nothing in the heap or free lists) and deletion touches no
// service state, so it is safe after the owning service is gone
// (the stale-entry path in try_pop_tl_cache deletes the same way).
struct tl_cache_owner
{
    ~tl_cache_owner()
    {
        delete tl_cached_impl.get();
        tl_cached_impl.set(nullptr);

        delete tl_cached_waiter.get();
        tl_cached_waiter.set(nullptr);
    }
};

inline void
arm_tl_cache_cleanup() noexcept
{
    thread_local tl_cache_owner owner;
    (void)owner;
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

inline waiter_node*
try_pop_waiter_tl_cache() noexcept
{
    auto* w = tl_cached_waiter.get();
    if (w)
    {
        tl_cached_waiter.set(nullptr);
        return w;
    }
    return nullptr;
}

inline bool
try_push_waiter_tl_cache(waiter_node* w) noexcept
{
    if (!tl_cached_waiter.get())
    {
        arm_tl_cache_cleanup();
        tl_cached_waiter.set(w);
        return true;
    }
    return false;
}

inline void
timer_service_invalidate_cache() noexcept
{
    delete tl_cached_impl.get();
    tl_cached_impl.set(nullptr);

    delete tl_cached_waiter.get();
    tl_cached_waiter.set(nullptr);
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
        while (auto* w = impl->waiters_.pop_front())
        {
            w->stop_cb_.reset();
            auto h = std::exchange(w->h_, {});
            sched_->work_finished();
            if (h)
                h.destroy();
            delete w;
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

    // Delete free-listed waiters
    while (waiter_free_list_)
    {
        auto* next = waiter_free_list_->next_free_;
        delete waiter_free_list_;
        waiter_free_list_ = next;
    }
}

inline io_object::implementation*
timer_service::construct()
{
    timer::implementation* impl = try_pop_tl_cache(this);
    if (impl)
    {
        impl->svc_ = this;
        impl->heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        impl->might_have_pending_waits_.store(false, std::memory_order_relaxed);
        return impl;
    }

    std::lock_guard lock(mutex_);
    if (free_list_)
    {
        impl              = free_list_;
        free_list_        = impl->next_free_;
        impl->next_free_  = nullptr;
        impl->svc_        = this;
        impl->heap_index_.store(
            (std::numeric_limits<std::size_t>::max)(),
            std::memory_order_relaxed);
        impl->might_have_pending_waits_.store(false, std::memory_order_relaxed);
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

inline waiter_node*
timer_service::create_waiter()
{
    if (auto* w = try_pop_waiter_tl_cache())
        return w;

    std::lock_guard lock(mutex_);
    if (waiter_free_list_)
    {
        auto* w           = waiter_free_list_;
        waiter_free_list_ = w->next_free_;
        w->next_free_     = nullptr;
        return w;
    }

    return new waiter_node();
}

inline void
timer_service::destroy_waiter(waiter_node* w)
{
    if (try_push_waiter_tl_cache(w))
        return;

    std::lock_guard lock(mutex_);
    w->next_free_     = waiter_free_list_;
    waiter_free_list_ = w;
}

inline std::size_t
timer_service::update_timer(timer::implementation& impl, time_point new_time)
{
    // Gate on the flag, not waiters_: reading the non-atomic list
    // here would race a concurrent drain. A false flag is safe to
    // trust pre-lock because every draining critical section stores
    // it false as its final touch of the impl.
    bool in_heap =
        (impl.heap_index_.load(std::memory_order_relaxed) !=
         (std::numeric_limits<std::size_t>::max)());
    if (!in_heap &&
        !impl.might_have_pending_waits_.load(std::memory_order_relaxed))
        return 0;

    bool notify = false;
    intrusive_list<waiter_node> canceled;

    {
        std::lock_guard lock(mutex_);

        while (auto* w = impl.waiters_.pop_front())
        {
            w->impl_ = nullptr;
            canceled.push_back(w);
        }

        std::size_t idx = impl.heap_index_.load(std::memory_order_relaxed);
        if (idx < heap_.size())
        {
            time_point old_time = heap_[idx].time_;
            heap_[idx].time_    = new_time;

            if (new_time < old_time)
                up_heap(idx);
            else
                down_heap(idx);

            notify =
                (impl.heap_index_.load(std::memory_order_relaxed) == 0);
        }

        refresh_cached_nearest();
    }

    std::size_t count = 0;
    while (auto* w = canceled.pop_front())
    {
        w->ec_value_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
        ++count;
    }

    if (notify)
        on_earliest_changed_();

    return count;
}

inline void
timer_service::insert_waiter(timer::implementation& impl, waiter_node* w)
{
    bool notify      = false;
    bool lost_cancel = false;
    {
        std::lock_guard lock(mutex_);
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
        impl.waiters_.push_back(w);

        // Lost-cancel re-check: a stop requested after the canceller was
        // armed in wait() but before this publication found impl_ null
        // and returned a no-op. Observe it now and undo the insertion.
        if (w->token_.stop_requested())
        {
            w->impl_ = nullptr;
            impl.waiters_.remove(w);
            if (impl.waiters_.empty())
            {
                remove_timer_impl(impl);
                impl.might_have_pending_waits_.store(
                    false, std::memory_order_relaxed);
            }
            refresh_cached_nearest();
            lost_cancel = true;
            notify      = false; // insertion undone; nearest unchanged
        }
    }
    if (notify)
        on_earliest_changed_();
    if (lost_cancel)
    {
        w->ec_value_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
    }
}

inline std::size_t
timer_service::cancel_timer(timer::implementation& impl)
{
    if (!impl.might_have_pending_waits_.load(std::memory_order_relaxed))
        return 0;

    // No unlocked already-done fast-out here: it would need the
    // non-atomic waiters_ (a race with concurrent drains), and an
    // index-only check is lifetime-unsafe because npos is stored
    // before the drain finishes touching the impl. A stale-true
    // flag is rare with the stateless API; the locked path below
    // re-validates.

    intrusive_list<waiter_node> canceled;

    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
        while (auto* w = impl.waiters_.pop_front())
        {
            w->impl_ = nullptr;
            canceled.push_back(w);
        }
        // Store false as the final touch of the impl under the lock so
        // update_timer's pre-lock false-flag trust holds unqualified.
        impl.might_have_pending_waits_.store(false, std::memory_order_relaxed);
        refresh_cached_nearest();
    }

    std::size_t count = 0;
    while (auto* w = canceled.pop_front())
    {
        w->ec_value_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
        ++count;
    }

    return count;
}

inline void
timer_service::cancel_waiter(waiter_node* w)
{
    {
        std::lock_guard lock(mutex_);
        // Already removed by cancel_timer or process_expired
        if (!w->impl_)
            return;
        auto* impl = w->impl_;
        w->impl_   = nullptr;
        impl->waiters_.remove(w);
        if (impl->waiters_.empty())
        {
            remove_timer_impl(*impl);
            impl->might_have_pending_waits_.store(
                false, std::memory_order_relaxed);
        }
        refresh_cached_nearest();
    }

    w->ec_value_ = make_error_code(capy::error::canceled);
    sched_->post(&w->op_);
}

inline std::size_t
timer_service::cancel_one_waiter(timer::implementation& impl)
{
    if (!impl.might_have_pending_waits_.load(std::memory_order_relaxed))
        return 0;

    waiter_node* w = nullptr;

    {
        std::lock_guard lock(mutex_);
        w = impl.waiters_.pop_front();
        if (!w)
            return 0;
        w->impl_ = nullptr;
        if (impl.waiters_.empty())
        {
            remove_timer_impl(impl);
            impl.might_have_pending_waits_.store(
                false, std::memory_order_relaxed);
        }
        refresh_cached_nearest();
    }

    w->ec_value_ = make_error_code(capy::error::canceled);
    sched_->post(&w->op_);
    return 1;
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
            while (auto* w = t->waiters_.pop_front())
            {
                w->impl_     = nullptr;
                w->ec_value_ = {};
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

// waiter_node out-of-class member function definitions

inline void
waiter_node::canceller::operator()() const
{
    waiter_->svc_->cancel_waiter(waiter_);
}

inline void
waiter_node::completion_op::do_complete(
    [[maybe_unused]] void* owner,
    scheduler_op* base,
    std::uint32_t,
    std::uint32_t)
{
    // owner is always non-null here. The destroy path (owner == nullptr)
    // is unreachable because completion_op overrides destroy() directly,
    // bypassing scheduler_op::destroy() which would call func_(nullptr, ...).
    BOOST_COROSIO_ASSERT(owner);
    static_cast<completion_op*>(base)->operator()();
}

inline void
waiter_node::completion_op::operator()()
{
    auto* w = waiter_;
    w->stop_cb_.reset();
    if (w->ec_out_)
        *w->ec_out_ = w->ec_value_;

    auto* cont  = w->cont_;
    auto d      = w->d_;
    auto* svc   = w->svc_;
    auto& sched = svc->get_scheduler();

    svc->destroy_waiter(w);

    d.post(*cont);
    sched.work_finished();
}

// GCC 14 false-positive: inlining ~optional<stop_callback> through
// delete loses track that stop_cb_ was already .reset() above.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
inline void
waiter_node::completion_op::destroy()
{
    // Called during scheduler shutdown drain when this completion_op is
    // in the scheduler's ready queue (posted by cancel_timer() or
    // process_expired()). Balances the work_started() from
    // implementation::wait(). The scheduler drain loop separately
    // balances the work_started() from post(). On IOCP both decrements
    // are required for outstanding_work_ to reach zero; on other
    // backends this is harmless.
    //
    // This override also prevents scheduler_op::destroy() from calling
    // do_complete(nullptr, ...). See also: timer_service::shutdown()
    // which drains waiters still in the timer heap (the other path).
    auto* w = waiter_;
    w->stop_cb_.reset();
    auto h      = std::exchange(w->h_, {});
    auto& sched = w->svc_->get_scheduler();
    delete w;
    sched.work_finished();
    if (h)
        h.destroy();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

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

inline std::size_t
timer_service_update_expiry(timer::implementation& impl)
{
    return impl.svc_->update_timer(impl, impl.expiry_);
}

inline std::size_t
timer_service_cancel(timer::implementation& impl) noexcept
{
    return impl.svc_->cancel_timer(impl);
}

inline std::size_t
timer_service_cancel_one(timer::implementation& impl) noexcept
{
    return impl.svc_->cancel_one_waiter(impl);
}

inline timer_service&
get_timer_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<timer_service>(sched);
}

} // namespace boost::corosio::detail

#endif
