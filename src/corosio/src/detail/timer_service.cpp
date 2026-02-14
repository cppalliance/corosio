//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/timer_service.hpp"
#include "src/detail/scheduler_impl.hpp"

#include <boost/corosio/basic_io_context.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>
#include "src/detail/scheduler_op.hpp"
#include "src/detail/intrusive.hpp"
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <system_error>

#include <atomic>
#include <coroutine>
#include <limits>
#include <mutex>
#include <optional>
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
    waiter_node holds per-waiter state: coroutine handle, executor,
    error output, stop_token, embedded completion_op. Each concurrent
    co_await t.wait() allocates one waiter_node.

    timer_impl holds per-timer state: expiry, heap index, and an
    intrusive_list of waiter_nodes. Multiple coroutines can wait on
    the same timer simultaneously.

    timer_service_impl owns a min-heap of active timers, a free list
    of recycled impls, and a free list of recycled waiter_nodes. The
    heap is ordered by expiry time; the scheduler queries
    nearest_expiry() to set the epoll/timerfd timeout.

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
       create-then-destroy-on-same-thread pattern. On pop, if the
       cached impl's svc_ doesn't match the current service, the
       stale impl is deleted eagerly rather than reused.

    3. Embedded completion_op — Each waiter_node embeds a
       scheduler_op subclass, eliminating heap allocation per
       fire/cancel. Its destroy() is a no-op since the waiter_node
       owns the lifetime.

    4. Cached nearest expiry — An atomic<int64_t> mirrors the heap
       root's time, updated under the lock. nearest_expiry() and
       empty() read the atomic without locking.

    5. might_have_pending_waits_ flag — Set on wait(), cleared on
       cancel. Lets cancel_timer() return without locking when no
       wait was ever issued.

    6. Thread-local waiter cache — Single-slot per-thread cache of
       waiter_node avoids the free-list mutex for the common
       wait-then-complete-on-same-thread pattern.

    With all fast paths hit (idle timer, same thread), the
    schedule/cancel cycle takes zero mutex locks.

    Concurrency
    -----------
    stop_token callbacks can fire from any thread. The impl_
    pointer on waiter_node is used as a "still in list" marker:
    set to nullptr under the mutex when a waiter is removed by
    cancel_timer() or process_expired(). cancel_waiter() checks
    this under the mutex to avoid double-removal races.

    Multiple io_contexts in the same program are safe. The
    service pointer is obtained directly from the scheduler,
    and TL-cached impls are validated by comparing svc_ against
    the current service pointer. Waiter nodes have no service
    affinity and can safely migrate between contexts.
*/

namespace boost::corosio::detail {

class timer_service_impl;
struct timer_impl;
struct waiter_node;

void timer_service_invalidate_cache() noexcept;

struct waiter_node
    : intrusive_list<waiter_node>::node
{
    // Embedded completion op — avoids heap allocation per fire/cancel
    struct completion_op final : scheduler_op
    {
        waiter_node* waiter_ = nullptr;

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
        // No-op — lifetime owned by waiter_node, not the scheduler queue
        void destroy() override {}
    };

    // Per-waiter stop_token cancellation
    struct canceller
    {
        waiter_node* waiter_;
        void operator()() const;
    };

    // nullptr once removed from timer's waiter list (concurrency marker)
    timer_impl* impl_ = nullptr;
    timer_service_impl* svc_ = nullptr;
    std::coroutine_handle<> h_;
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

struct timer_impl
    : timer::timer_impl
{
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration = clock_type::duration;

    timer_service_impl* svc_ = nullptr;
    intrusive_list<waiter_node> waiters_;

    // Free list linkage (reused when impl is on free_list)
    timer_impl* next_free_ = nullptr;

    explicit timer_impl(timer_service_impl& svc) noexcept;


    void release() override;

    std::coroutine_handle<> wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*) override;
};

timer_impl* try_pop_tl_cache(timer_service_impl*) noexcept;
bool try_push_tl_cache(timer_impl*) noexcept;
waiter_node* try_pop_waiter_tl_cache() noexcept;
bool try_push_waiter_tl_cache(waiter_node*) noexcept;

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
    waiter_node* waiter_free_list_ = nullptr;
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
            while (auto* w = impl->waiters_.pop_front())
            {
                w->stop_cb_.reset();
                w->h_.destroy();
                sched_->on_work_finished();
                delete w;
            }
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
            delete impl;
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

        // Delete free-listed waiters
        while (waiter_free_list_)
        {
            auto* next = waiter_free_list_->next_free_;
            delete waiter_free_list_;
            waiter_free_list_ = next;
        }
    }

    io_object::io_object_impl* construct() override
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
            impl->svc_ = this;
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
            impl->might_have_pending_waits_ = false;
        }
        else
        {
            impl = new timer_impl(*this);
        }
        return impl;
    }

    void destroy(io_object::io_object_impl* p) override
    {
        static_cast<timer_impl*>(p)->release();
    }

    void destroy_impl(timer_impl& impl)
    {
        cancel_timer(impl);

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
    }

    waiter_node* create_waiter()
    {
        if (auto* w = try_pop_waiter_tl_cache())
            return w;

        std::lock_guard lock(mutex_);
        if (waiter_free_list_)
        {
            auto* w = waiter_free_list_;
            waiter_free_list_ = w->next_free_;
            w->next_free_ = nullptr;
            return w;
        }

        return new waiter_node();
    }

    void destroy_waiter(waiter_node* w)
    {
        if (try_push_waiter_tl_cache(w))
            return;

        std::lock_guard lock(mutex_);
        w->next_free_ = waiter_free_list_;
        waiter_free_list_ = w;
    }

    // Heap insertion deferred to wait() — avoids lock when timer is idle
    std::size_t update_timer(timer_impl& impl, time_point new_time)
    {
        bool in_heap =
            (impl.heap_index_ != (std::numeric_limits<std::size_t>::max)());
        if (!in_heap && impl.waiters_.empty())
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

    // Inserts timer into heap if needed and pushes waiter, all under
    // one lock to prevent races with cancel_waiter/process_expired
    void insert_waiter(timer_impl& impl, waiter_node* w)
    {
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            if (impl.heap_index_ == (std::numeric_limits<std::size_t>::max)())
            {
                impl.heap_index_ = heap_.size();
                heap_.push_back({impl.expiry_, &impl});
                up_heap(heap_.size() - 1);
                notify = (impl.heap_index_ == 0);
                refresh_cached_nearest();
            }
            impl.waiters_.push_back(w);
        }
        if (notify)
            on_earliest_changed_();
    }

    std::size_t cancel_timer(timer_impl& impl)
    {
        if (!impl.might_have_pending_waits_)
            return 0;

        // Not in heap and no waiters — just clear the flag
        if (impl.heap_index_ == (std::numeric_limits<std::size_t>::max)()
            && impl.waiters_.empty())
        {
            impl.might_have_pending_waits_ = false;
            return 0;
        }

        intrusive_list<waiter_node> canceled;

        {
            std::lock_guard lock(mutex_);
            remove_timer_impl(impl);
            while (auto* w = impl.waiters_.pop_front())
            {
                w->impl_ = nullptr;
                canceled.push_back(w);
            }
            refresh_cached_nearest();
        }

        impl.might_have_pending_waits_ = false;

        std::size_t count = 0;
        while (auto* w = canceled.pop_front())
        {
            w->ec_value_ = make_error_code(capy::error::canceled);
            sched_->post(&w->op_);
            ++count;
        }

        return count;
    }

    // Cancel a single waiter (called from stop_token callback, any thread)
    void cancel_waiter(waiter_node* w)
    {
        {
            std::lock_guard lock(mutex_);
            // Already removed by cancel_timer or process_expired
            if (!w->impl_)
                return;
            auto* impl = w->impl_;
            w->impl_ = nullptr;
            impl->waiters_.remove(w);
            if (impl->waiters_.empty())
            {
                remove_timer_impl(*impl);
                impl->might_have_pending_waits_ = false;
            }
            refresh_cached_nearest();
        }

        w->ec_value_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
    }

    // Cancel front waiter only (FIFO), return 0 or 1
    std::size_t cancel_one_waiter(timer_impl& impl)
    {
        if (!impl.might_have_pending_waits_)
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
                impl.might_have_pending_waits_ = false;
            }
            refresh_cached_nearest();
        }

        w->ec_value_ = make_error_code(capy::error::canceled);
        sched_->post(&w->op_);
        return 1;
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
        intrusive_list<waiter_node> expired;

        {
            std::lock_guard lock(mutex_);
            auto now = clock_type::now();

            while (!heap_.empty() && heap_[0].time_ <= now)
            {
                timer_impl* t = heap_[0].timer_;
                remove_timer_impl(*t);
                while (auto* w = t->waiters_.pop_front())
                {
                    w->impl_ = nullptr;
                    w->ec_value_ = {};
                    expired.push_back(w);
                }
                t->might_have_pending_waits_ = false;
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

timer_impl::
timer_impl(timer_service_impl& svc) noexcept
    : svc_(&svc)
{
}

void
waiter_node::canceller::
operator()() const
{
    waiter_->svc_->cancel_waiter(waiter_);
}

void
waiter_node::completion_op::
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
waiter_node::completion_op::
operator()()
{
    auto* w = waiter_;
    w->stop_cb_.reset();
    if (w->ec_out_)
        *w->ec_out_ = w->ec_value_;

    auto h = w->h_;
    auto d = w->d_;
    auto* svc = w->svc_;
    auto& sched = svc->get_scheduler();

    svc->destroy_waiter(w);

    d.post(h);
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
    // Already-expired fast path — no waiter_node, no mutex.
    // Post instead of dispatch so the coroutine yields to the
    // scheduler, allowing other queued work to run.
    if (heap_index_ == (std::numeric_limits<std::size_t>::max)())
    {
        if (expiry_ == (time_point::min)() ||
            expiry_ <= clock_type::now())
        {
            if (ec)
                *ec = {};
            d.post(h);
            return std::noop_coroutine();
        }
    }

    auto* w = svc_->create_waiter();
    w->impl_ = this;
    w->svc_ = svc_;
    w->h_ = h;
    w->d_ = std::move(d);
    w->token_ = std::move(token);
    w->ec_out_ = ec;

    svc_->insert_waiter(*this, w);
    might_have_pending_waits_ = true;
    svc_->get_scheduler().on_work_started();

    if (w->token_.stop_possible())
        w->stop_cb_.emplace(w->token_, waiter_node::canceller{w});

    return std::noop_coroutine();
}

// Extern free functions called from timer.cpp
//
// Two thread-local caches avoid hot-path mutex acquisitions:
//
// 1. Impl cache — single-slot, validated by comparing svc_ on the
//    impl against the current service pointer.
//
// 2. Waiter cache — single-slot, no service affinity.
//
// The service pointer is obtained from the scheduler_impl's
// timer_svc_ member, avoiding find_service() on the hot path.
// All caches are cleared by timer_service_invalidate_cache()
// during shutdown.

thread_local_ptr<timer_impl> tl_cached_impl;
thread_local_ptr<waiter_node> tl_cached_waiter;

timer_impl*
try_pop_tl_cache(timer_service_impl* svc) noexcept
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

bool
try_push_tl_cache(timer_impl* impl) noexcept
{
    if (!tl_cached_impl.get())
    {
        tl_cached_impl.set(impl);
        return true;
    }
    return false;
}

waiter_node*
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

bool
try_push_waiter_tl_cache(waiter_node* w) noexcept
{
    if (!tl_cached_waiter.get())
    {
        tl_cached_waiter.set(w);
        return true;
    }
    return false;
}

void
timer_service_invalidate_cache() noexcept
{
    delete tl_cached_impl.get();
    tl_cached_impl.set(nullptr);

    delete tl_cached_waiter.get();
    tl_cached_waiter.set(nullptr);
}

struct timer_service_access
{
    static scheduler_impl& get_scheduler(basic_io_context& ctx) noexcept
    {
        return static_cast<scheduler_impl&>(*ctx.sched_);
    }
};

std::size_t
timer_service_update_expiry(timer::timer_impl& base)
{
    auto& impl = static_cast<timer_impl&>(base);
    return impl.svc_->update_timer(impl, impl.expiry_);
}

std::size_t
timer_service_cancel(timer::timer_impl& base) noexcept
{
    auto& impl = static_cast<timer_impl&>(base);
    return impl.svc_->cancel_timer(impl);
}

std::size_t
timer_service_cancel_one(timer::timer_impl& base) noexcept
{
    auto& impl = static_cast<timer_impl&>(base);
    return impl.svc_->cancel_one_waiter(impl);
}

timer_service&
get_timer_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<timer_service_impl>(sched);
}

} // namespace boost::corosio::detail
