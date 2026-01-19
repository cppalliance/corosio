//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "timer_service.hpp"

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/core/intrusive_list.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/any_coro.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace boost {
namespace corosio {
namespace detail {

class timer_service_impl;

struct timer_impl
    : timer::timer_impl
    , capy::intrusive_list<timer_impl>::node
{
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration = clock_type::duration;

    timer_service_impl* svc_ = nullptr;
    time_point expiry_;
    std::size_t heap_index_ = (std::numeric_limits<std::size_t>::max)();

    // Wait operation state
    std::coroutine_handle<> h_;
    capy::any_executor_ref d_;
    system::error_code* ec_out_ = nullptr;
    std::stop_token token_;
    bool waiting_ = false;

    explicit timer_impl(timer_service_impl& svc) noexcept
        : svc_(&svc)
    {
    }

    void release() override;

    void wait(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        std::stop_token,
        system::error_code*) override;
};

//------------------------------------------------------------------------------

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
    capy::intrusive_list<timer_impl> timers_;
    capy::intrusive_list<timer_impl> free_list_;
    callback on_earliest_changed_;

public:
    timer_service_impl(capy::execution_context&, scheduler& sched)
        : timer_service()
        , sched_(&sched)
    {
    }

    scheduler& get_scheduler() noexcept { return *sched_; }

    ~timer_service_impl()
    {
    }

    timer_service_impl(timer_service_impl const&) = delete;
    timer_service_impl& operator=(timer_service_impl const&) = delete;

    void set_on_earliest_changed(callback cb) override
    {
        on_earliest_changed_ = cb;
    }

    void shutdown() override
    {
        while (auto* impl = timers_.pop_front())
            delete impl;
        while (auto* impl = free_list_.pop_front())
            delete impl;
    }

    timer::timer_impl* create_impl() override
    {
        std::lock_guard lock(mutex_);
        timer_impl* impl;
        if (auto* p = free_list_.pop_front())
        {
            impl = p;
            impl->heap_index_ = (std::numeric_limits<std::size_t>::max)();
        }
        else
        {
            impl = new timer_impl(*this);
        }
        timers_.push_back(impl);
        return impl;
    }

    void destroy_impl(timer_impl& impl)
    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
        timers_.remove(&impl);
        free_list_.push_back(&impl);
    }

    void update_timer(timer_impl& impl, time_point new_time)
    {
        bool notify = false;
        bool was_waiting = false;
        std::coroutine_handle<> h;
        capy::any_executor_ref d;
        system::error_code* ec_out = nullptr;

        {
            std::lock_guard lock(mutex_);

            // If currently waiting, cancel the pending wait
            if (impl.waiting_)
            {
                was_waiting = true;
                impl.waiting_ = false;
                h = impl.h_;
                d = impl.d_;
                ec_out = impl.ec_out_;
            }

            if (impl.heap_index_ < heap_.size())
            {
                // Already in heap, update position
                time_point old_time = heap_[impl.heap_index_].time_;
                heap_[impl.heap_index_].time_ = new_time;

                if (new_time < old_time)
                    up_heap(impl.heap_index_);
                else
                    down_heap(impl.heap_index_);
            }
            else
            {
                // Not in heap, add it
                impl.heap_index_ = heap_.size();
                heap_.push_back({new_time, &impl});
                up_heap(heap_.size() - 1);
            }

            // Notify if this timer is now the earliest
            notify = (impl.heap_index_ == 0);
        }

        // Resume cancelled waiter outside lock
        if (was_waiting)
        {
            if (ec_out)
                *ec_out = make_error_code(capy::error::canceled);
            auto resume_h = d.dispatch(h);
            // Resume the handle if executor returned it for symmetric transfer
            if (resume_h.address() == h.address())
                resume_h.resume();
            // Call on_work_finished AFTER the coroutine resumes
            sched_->on_work_finished();
        }

        if (notify)
            on_earliest_changed_();
    }

    void remove_timer(timer_impl& impl)
    {
        std::lock_guard lock(mutex_);
        remove_timer_impl(impl);
    }

    void cancel_timer(timer_impl& impl)
    {
        std::coroutine_handle<> h;
        capy::any_executor_ref d;
        system::error_code* ec_out = nullptr;
        bool was_waiting = false;

        {
            std::lock_guard lock(mutex_);
            remove_timer_impl(impl);
            if (impl.waiting_)
            {
                was_waiting = true;
                impl.waiting_ = false;
                h = impl.h_;
                d = std::move(impl.d_);
                ec_out = impl.ec_out_;
            }
        }

        // Dispatch outside lock
        if (was_waiting)
        {
            if (ec_out)
                *ec_out = make_error_code(capy::error::canceled);
            auto resume_h = d.dispatch(h);
            // Resume the handle if executor returned it for symmetric transfer
            if (resume_h.address() == h.address())
                resume_h.resume();
            // Call on_work_finished AFTER the coroutine resumes
            sched_->on_work_finished();
        }
    }

    bool empty() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return heap_.empty();
    }

    time_point nearest_expiry() const noexcept override
    {
        std::lock_guard lock(mutex_);
        return heap_.empty() ? time_point::max() : heap_[0].time_;
    }

    std::size_t process_expired() override
    {
        // Collect expired timers while holding lock
        struct expired_entry
        {
            std::coroutine_handle<> h;
            capy::any_executor_ref d;
            system::error_code* ec_out;
        };
        std::vector<expired_entry> expired;

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
                    expired.push_back({t->h_, std::move(t->d_), t->ec_out_});
                }
                // If not waiting, timer is removed but not dispatched -
                // wait() will handle this by checking expiry
            }
        }

        // Dispatch outside lock
        for (auto& e : expired)
        {
            if (e.ec_out)
                *e.ec_out = {};
            auto resume_h = e.d.dispatch(e.h);
            // Resume the handle if executor returned it for symmetric transfer
            // (executor returns our handle if we should resume, noop if it posted)
            if (resume_h.address() == e.h.address())
                resume_h.resume();
            // Call on_work_finished AFTER the coroutine resumes, so it has a
            // chance to add new work before we potentially trigger stop()
            sched_->on_work_finished();
        }

        return expired.size();
    }

private:
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

//------------------------------------------------------------------------------

void
timer_impl::
release()
{
    svc_->destroy_impl(*this);
}

void
timer_impl::
wait(
    std::coroutine_handle<> h,
    capy::any_executor_ref d,
    std::stop_token token,
    system::error_code* ec)
{
    // Check if timer already expired (not in heap anymore)
    bool already_expired = (heap_index_ == (std::numeric_limits<std::size_t>::max)());

    if (already_expired)
    {
        // Timer already expired - dispatch immediately
        if (ec)
            *ec = {};
        // Note: no work tracking needed - we dispatch synchronously
        auto resume_h = d.dispatch(h);
        // Resume the handle if executor returned it for symmetric transfer
        if (resume_h.address() == h.address())
            resume_h.resume();
        return;
    }

    h_ = h;
    d_ = std::move(d);
    token_ = std::move(token);
    ec_out_ = ec;
    waiting_ = true;
    svc_->get_scheduler().on_work_started();
}

//------------------------------------------------------------------------------
//
// Extern free functions called from timer.cpp
//
//------------------------------------------------------------------------------

timer::timer_impl*
timer_service_create(capy::execution_context& ctx)
{
    auto* svc = ctx.find_service<timer_service>();
    if (!svc)
    {
        // Timer service not yet created - this happens if io_context
        // hasn't been constructed yet, or if the scheduler didn't
        // initialize the timer service
        throw std::runtime_error("timer_service not found");
    }
    return svc->create_impl();
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

} // namespace detail
} // namespace corosio
} // namespace boost
