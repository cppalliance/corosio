//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/timer_service.hpp"

#include <boost/corosio/detail/scheduler.hpp>
#include "src/detail/scheduler_op.hpp"
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <system_error>

#include <coroutine>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <vector>

/*
    Timer Service
    =============

    The public timer class holds an opaque timer_impl* and forwards
    all operations through extern free functions defined at the bottom
    of this file.

    Data Structures
    ---------------
    timer_impl holds per-timer state: expiry, coroutine handle,
    executor, embedded completion_op, heap index, and free-list link.

    timer_service_impl owns a min-heap of active timers and a free
    list of recycled impls. The heap is ordered by expiry time; the
    scheduler queries nearest_expiry() to set the epoll/timerfd
    timeout.

    Optimization Strategy
    ---------------------
    The common timer lifecycle is: construct, set expiry, cancel or
    wait, destroy. Several optimizations target this path:

    1. Deferred heap insertion — expires_after() stores the expiry
       but does not insert into the heap. Insertion happens in
       wait(). If the timer is cancelled or destroyed before wait(),
       the heap is never touched and no mutex is taken. This also
       enables the already-expired fast path: when wait() sees
       expiry <= now before inserting, it posts the coroutine
       handle to the executor and returns noop_coroutine — no
       heap, no mutex, no epoll. This is only possible because
       the coroutine API guarantees wait() always follows
       expires_after(); callback APIs cannot assume this call
       order.

    2. Thread-local impl cache — A single-slot per-thread cache of
       timer_impl avoids the mutex on create/destroy for the common
       create-then-destroy-on-same-thread pattern. The RAII wrapper
       tl_impl_cache deletes the cached impl when the thread exits.

    3. Thread-local service cache — Caches the {context, service}
       pair per-thread to skip find_service() on every timer
       construction.

    4. Embedded completion_op — timer_impl embeds a scheduler_op
       subclass, eliminating heap allocation per fire/cancel. Its
       destroy() is a no-op since the timer_impl owns the lifetime.

    5. Cached nearest expiry — An atomic<int64_t> mirrors the heap
       root's time, updated under the lock. nearest_expiry() and
       empty() read the atomic without locking.

    6. might_have_pending_waits_ flag — Set on wait(), cleared on
       cancel. Lets cancel_timer() return without locking when no
       wait was ever issued.

    With all fast paths hit (idle timer, same thread), the
    schedule/cancel cycle takes zero mutex locks.
*/

namespace boost::corosio::detail {

class timer_service_impl;

void timer_service_invalidate_cache() noexcept;

struct timer_impl
    : timer::timer_impl
{
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration = clock_type::duration;

    // Embedded completion op — avoids heap allocation per fire/cancel
    struct completion_op final : scheduler_op
    {
        timer_impl* impl_ = nullptr;

        static void do_complete(
            void* owner,
            scheduler_op* base,
            std::uint32_t,
            std::uint32_t);

        completion_op() noexcept
            : scheduler_op(&do_complete)
        {
        }

        void operator()() override;
        // No-op — lifetime owned by timer_impl, not the scheduler queue
        void destroy() override {}
    };

    timer_service_impl* svc_ = nullptr;
    time_point expiry_;
    std::size_t heap_index_ = (std::numeric_limits<std::size_t>::max)();
    // Lets cancel_timer() skip the lock when no wait() was ever issued
    bool might_have_pending_waits_ = false;

    // Wait operation state
    std::coroutine_handle<> h_;
    capy::executor_ref d_;
    std::error_code* ec_out_ = nullptr;
    std::stop_token token_;
    bool waiting_ = false;

    completion_op op_;
    std::error_code ec_value_;

    // Free list linkage (reused when impl is on free_list)
    timer_impl* next_free_ = nullptr;

    explicit timer_impl(timer_service_impl& svc) noexcept
        : svc_(&svc)
    {
        op_.impl_ = this;
    }

    void release() override;

    std::coroutine_handle<> wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*) override;
};

timer_impl* try_pop_tl_cache(timer_service_impl*) noexcept;
bool try_push_tl_cache(timer_impl*) noexcept;

class timer_service_impl : public timer_service
{
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using key_type = timer_service;

private:
    struct heap_entry
    {
        time_point time_;
        timer_impl* timer_;
    };

    scheduler* sched_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<heap_entry> heap_;
    timer_impl* free_list_ = nullptr;
    // Tracks impls not on free-list, for shutdown correctness
    std::size_t live_count_ = 0;
    callback on_earliest_changed_;
    // Avoids mutex in nearest_expiry() and empty()
    mutable std::atomic<std::int64_t> cached_nearest_ns_{
        (std::numeric_limits<std::int64_t>::max)()};

public:
    timer_service_impl(capy::execution_context&, scheduler& sched)
        : timer_service()
        , sched_(&sched)
    {
    }

    scheduler& get_scheduler() noexcept { return *sched_; }

    ~timer_service_impl() = default;

    timer_service_impl(timer_service_impl const&) = delete;
    timer_service_impl& operator=(timer_service_impl const&) = delete;

    void set_on_earliest_changed(callback cb) override
    {
        on_earliest_changed_ = cb;
    }

    void shutdown() override
    {
        timer_service_invalidate_cache();

        // Cancel waiting timers still in the heap
        for (auto& entry : heap_)
        {
            auto* impl = entry.timer_;
            if (impl->waiting_)
            {
                impl->waiting_ = false;
                impl->h_.destroy();
                sched_->on_work_finished();
            }
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
            delete impl;
            --live_count_;
        }
        heap_.clear();
        cached_nearest_ns_.store(
            (std::numeric_limits<std::int64_t>::max)(),
            std::memory_order_release);

        // Delete free-listed impls
        while (free_list_)
        {
            auto* next = free_list_->next_free_;
            delete free_list_;
            free_list_ = next;
        }

        // Any live timers not in heap and not on free list are still
        // referenced by timer objects — they'll call destroy_impl()
        // which will delete them (live_count_ tracks this).
    }

    timer::timer_impl* create_impl() override
    {
        timer_impl* impl = try_pop_tl_cache(this);
        if (impl)
        {
            impl->svc_ = this;
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
            impl->might_have_pending_waits_ = false;
            return impl;
        }

        std::lock_guard lock(mutex_);
        if (free_list_)
        {
            impl = free_list_;
            free_list_ = impl->next_free_;
            impl->next_free_ = nullptr;
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
            impl->might_have_pending_waits_ = false;
        }
        else
        {
            impl = new timer_impl(*this);
        }
        ++live_count_;
        return impl;
    }

    void destroy_impl(timer_impl& impl)
    {
        if (impl.heap_index_ != (std::numeric_limits<std::size_t>::max)())
        {
            std::lock_guard lock(mutex_);
            remove_timer_impl(impl);
            refresh_cached_nearest();
        }

        if (try_push_tl_cache(&impl))
            return;

        std::lock_guard lock(mutex_);
        impl.next_free_ = free_list_;
        free_list_ = &impl;
        --live_count_;
    }

    // Heap insertion deferred to wait() — avoids lock when timer is idle
    void update_timer(timer_impl& impl, time_point new_time)
    {
        bool in_heap =
            (impl.heap_index_ != (std::numeric_limits<std::size_t>::max)());
        if (!in_heap && !impl.waiting_)
            return;

        bool notify = false;
        bool was_waiting = false;

        {
            std::lock_guard lock(mutex_);

            if (impl.waiting_)
            {
                was_waiting = true;
                impl.waiting_ = false;
            }

            if (impl.heap_index_ < heap_.size())
            {
                time_point old_time = heap_[impl.heap_index_].time_;
                heap_[impl.heap_index_].time_ = new_time;

                if (new_time < old_time)
                    up_heap(impl.heap_index_);
                else
                    down_heap(impl.heap_index_);

                notify = (impl.heap_index_ == 0);
            }

            refresh_cached_nearest();
        }

        if (was_waiting)
        {
            impl.ec_value_ = make_error_code(capy::error::canceled);
            sched_->post(&impl.op_);
        }

        if (notify)
            on_earliest_changed_();
    }

    // Called from wait() when timer hasn't been inserted into the heap yet
    void insert_timer(timer_impl& impl)
    {
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            impl.heap_index_ = heap_.size();
            heap_.push_back({impl.expiry_, &impl});
            up_heap(heap_.size() - 1);
            notify = (impl.heap_index_ == 0);
            refresh_cached_nearest();
        }
        if (notify)
            on_earliest_changed_();
    }

    void cancel_timer(timer_impl& impl)
    {
        if (!impl.might_have_pending_waits_)
            return;

        // Not in heap and not waiting — just clear the flag
        if (impl.heap_index_ == (std::numeric_limits<std::size_t>::max)()
            && !impl.waiting_)
        {
            impl.might_have_pending_waits_ = false;
            return;
        }

        bool was_waiting = false;

        {
            std::lock_guard lock(mutex_);
            remove_timer_impl(impl);
            if (impl.waiting_)
            {
                was_waiting = true;
                impl.waiting_ = false;
            }
            refresh_cached_nearest();
        }

        impl.might_have_pending_waits_ = false;

        if (was_waiting)
        {
            impl.ec_value_ = make_error_code(capy::error::canceled);
            sched_->post(&impl.op_);
        }
    }

    bool empty() const noexcept override
    {
        return cached_nearest_ns_.load(std::memory_order_acquire)
            == (std::numeric_limits<std::int64_t>::max)();
    }

    time_point nearest_expiry() const noexcept override
    {
        auto ns = cached_nearest_ns_.load(std::memory_order_acquire);
        return time_point(time_point::duration(ns));
    }

    std::size_t process_expired() override
    {
        std::vector<timer_impl*> expired;

        {
            std::lock_guard lock(mutex_);
            auto now = clock_type::now();

            while (!heap_.empty() && heap_[0].time_ <= now)
            {
                timer_impl* t = heap_[0].timer_;
                remove_timer_impl(*t);

                if (t->waiting_)
                {
                    t->waiting_ = false;
                    t->ec_value_ = {};
                    expired.push_back(t);
                }
            }

            refresh_cached_nearest();
        }

        for (auto* t : expired)
            sched_->post(&t->op_);

        return expired.size();
    }

private:
    void refresh_cached_nearest() noexcept
    {
        auto ns = heap_.empty()
            ? (std::numeric_limits<std::int64_t>::max)()
            : heap_[0].time_.time_since_epoch().count();
        cached_nearest_ns_.store(ns, std::memory_order_release);
    }

    void remove_timer_impl(timer_impl& impl)
    {
        std::size_t index = impl.heap_index_;
        if (index >= heap_.size())
            return; // Not in heap

        if (index == heap_.size() - 1)
        {
            // Last element, just pop
            impl.heap_index_ = (std::numeric_limits<std::size_t>::max)();
            heap_.pop_back();
        }
        else
        {
            // Swap with last and reheapify
            swap_heap(index, heap_.size() - 1);
            impl.heap_index_ = (std::numeric_limits<std::size_t>::max)();
            heap_.pop_back();

            if (index > 0 && heap_[index].time_ < heap_[(index - 1) / 2].time_)
                up_heap(index);
            else
                down_heap(index);
        }
    }

    void up_heap(std::size_t index)
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

    void down_heap(std::size_t index)
    {
        std::size_t child = index * 2 + 1;
        while (child < heap_.size())
        {
            std::size_t min_child = (child + 1 == heap_.size() ||
                heap_[child].time_ < heap_[child + 1].time_)
                ? child : child + 1;

            if (heap_[index].time_ < heap_[min_child].time_)
                break;

            swap_heap(index, min_child);
            index = min_child;
            child = index * 2 + 1;
        }
    }

    void swap_heap(std::size_t i1, std::size_t i2)
    {
        heap_entry tmp = heap_[i1];
        heap_[i1] = heap_[i2];
        heap_[i2] = tmp;
        heap_[i1].timer_->heap_index_ = i1;
        heap_[i2].timer_->heap_index_ = i2;
    }
};

void
timer_impl::completion_op::
do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t,
    std::uint32_t)
{
    if (!owner)
        return;
    static_cast<completion_op*>(base)->operator()();
}

void
timer_impl::completion_op::
operator()()
{
    auto* impl = impl_;
    if (impl->ec_out_)
        *impl->ec_out_ = impl->ec_value_;

    auto& sched = impl->svc_->get_scheduler();
    impl->d_.post(impl->h_);
    sched.on_work_finished();
}

void
timer_impl::
release()
{
    svc_->destroy_impl(*this);
}

std::coroutine_handle<>
timer_impl::
wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec)
{
    if (heap_index_ == (std::numeric_limits<std::size_t>::max)())
    {
        if (expiry_ <= clock_type::now())
        {
            if (ec)
                *ec = {};
            d.post(h);
            return std::noop_coroutine();
        }

        svc_->insert_timer(*this);
    }

    h_ = h;
    d_ = std::move(d);
    token_ = std::move(token);
    ec_out_ = ec;
    waiting_ = true;
    might_have_pending_waits_ = true;
    svc_->get_scheduler().on_work_started();
    return std::noop_coroutine();
}

// Extern free functions called from timer.cpp
//
// Thread-local caches invalidated by timer_service_invalidate_cache()
// during shutdown. The service cache avoids find_service overhead per
// timer construction. The impl cache avoids the free-list mutex for
// the common create-then-destroy-on-same-thread pattern.
static thread_local capy::execution_context* cached_ctx = nullptr;
static thread_local timer_service_impl* cached_svc = nullptr;

// RAII wrapper deletes the cached impl when the thread exits
struct tl_impl_cache
{
    timer_impl* ptr = nullptr;
    ~tl_impl_cache() { delete ptr; }
};
static thread_local tl_impl_cache tl_cached_impl;

timer_impl*
try_pop_tl_cache(timer_service_impl* svc) noexcept
{
    if (tl_cached_impl.ptr && tl_cached_impl.ptr->svc_ == svc)
    {
        auto* impl = tl_cached_impl.ptr;
        tl_cached_impl.ptr = nullptr;
        return impl;
    }
    return nullptr;
}

bool
try_push_tl_cache(timer_impl* impl) noexcept
{
    if (!tl_cached_impl.ptr)
    {
        tl_cached_impl.ptr = impl;
        return true;
    }
    return false;
}

void
timer_service_invalidate_cache() noexcept
{
    cached_ctx = nullptr;
    cached_svc = nullptr;
    delete tl_cached_impl.ptr;
    tl_cached_impl.ptr = nullptr;
}

timer::timer_impl*
timer_service_create(capy::execution_context& ctx)
{
    if (cached_ctx != &ctx)
    {
        cached_svc = static_cast<timer_service_impl*>(
            ctx.find_service<timer_service>());
        if (!cached_svc)
            throw std::runtime_error("timer_service not found");
        cached_ctx = &ctx;
    }
    return cached_svc->create_impl();
}

void
timer_service_destroy(timer::timer_impl& base) noexcept
{
    static_cast<timer_impl&>(base).release();
}

timer::time_point
timer_service_expiry(timer::timer_impl& base) noexcept
{
    return static_cast<timer_impl&>(base).expiry_;
}

void
timer_service_expires_at(timer::timer_impl& base, timer::time_point t)
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.expiry_ = t;
    impl.svc_->update_timer(impl, t);
}

void
timer_service_expires_after(timer::timer_impl& base, timer::duration d)
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.expiry_ = timer::clock_type::now() + d;
    impl.svc_->update_timer(impl, impl.expiry_);
}

void
timer_service_cancel(timer::timer_impl& base) noexcept
{
    auto& impl = static_cast<timer_impl&>(base);
    impl.svc_->cancel_timer(impl);
}

timer_service&
get_timer_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<timer_service_impl>(sched);
}

} // namespace boost::corosio::detail
