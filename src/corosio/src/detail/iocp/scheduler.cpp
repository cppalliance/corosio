//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/iocp/overlapped_op.hpp"
#include "src/detail/iocp/timers.hpp"
#include "src/detail/timer_service.hpp"
#include "src/detail/iocp/resolver_service.hpp"
#include "src/detail/make_err.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <limits>

/*
    ARCHITECTURE NOTE: Function Pointer Dispatch

    All I/O handles are registered with the IOCP using key_io (0).
    Dispatch happens via the function pointer stored in each scheduler_op.
    
    When GQCS returns with an OVERLAPPED*, we cast it to scheduler_op*
    and call the function pointer directly - no virtual dispatch.

    The completion_key enum values are used only for internal signals:
      - key_io (0): Normal I/O completion, dispatch via func_
      - key_wake_dispatch (1): Timer wakeup, check dispatch_required_
      - key_shutdown (2): Stop signal
      - key_result_stored (3): Results pre-stored in OVERLAPPED
*/

namespace boost::corosio::detail {

namespace {

// Max timeout for GQCS to allow periodic re-checking of conditions
constexpr unsigned long max_gqcs_timeout = 500;

struct scheduler_context
{
    win_scheduler const* key;
    scheduler_context* next;
};

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        win_scheduler const* ctx) noexcept
        : frame_{ctx, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        context_stack.set(frame_.next);
    }
};

} // namespace

win_scheduler::
win_scheduler(
    capy::execution_context& ctx,
    int concurrency_hint)
    : iocp_(nullptr)
    , outstanding_work_(0)
    , stopped_(0)
    , shutdown_(0)
    , stop_event_posted_(0)
    , dispatch_required_(0)
{
    // concurrency_hint < 0 means use system default (DWORD(~0) = max)
    iocp_ = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE,
        nullptr,
        0,
        static_cast<DWORD>(concurrency_hint >= 0 ? concurrency_hint : DWORD(~0)));

    if (iocp_ == nullptr)
        detail::throw_system_error(make_err(::GetLastError()));

    // Create timer wakeup mechanism (tries NT native, falls back to thread)
    timers_ = make_win_timers(iocp_, &dispatch_required_);

    // Connect timer service to scheduler
    set_timer_service(&get_timer_service(ctx, *this));

    // Initialize resolver service
    ctx.make_service<win_resolver_service>(*this);
}

win_scheduler::
~win_scheduler()
{
    if (iocp_ != nullptr)
        ::CloseHandle(iocp_);
}

void
win_scheduler::
shutdown()
{
    ::InterlockedExchange(&shutdown_, 1);

    // Stop timer wakeup mechanism
    if (timers_)
        timers_->stop();

    // Drain all outstanding operations without invoking handlers
    while (::InterlockedExchangeAdd(&outstanding_work_, 0) > 0)
    {
        // First drain the fallback queue
        op_queue ops;
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            ops.splice(completed_ops_);
        }

        while (auto* h = ops.pop())
        {
            ::InterlockedDecrement(&outstanding_work_);
            h->destroy();
        }

        // Then drain from IOCP with zero timeout (non-blocking)
        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;
        ::GetQueuedCompletionStatus(iocp_, &bytes, &key, &overlapped, 0);
        if (overlapped)
        {
            ::InterlockedDecrement(&outstanding_work_);
            if (key == key_posted)
            {
                // Posted scheduler_op*
                auto* op = reinterpret_cast<scheduler_op*>(overlapped);
                op->destroy();
            }
            else
            {
                // Actual I/O: convert OVERLAPPED* to overlapped_op*
                auto* op = overlapped_to_op(overlapped);
                op->destroy();
            }
        }
    }
}

void
win_scheduler::
post(capy::coro h) const
{
    struct post_handler final : scheduler_op
    {
        capy::coro h_;

        static void do_complete(
            void* owner,
            scheduler_op* base,
            std::uint32_t,
            std::uint32_t)
        {
            auto* self = static_cast<post_handler*>(base);
            if (!owner)
            {
                // Destroy path
                delete self;
                return;
            }
            auto coro = self->h_;
            delete self;
            std::atomic_thread_fence(std::memory_order_acquire);
            coro.resume();
        }

        explicit post_handler(capy::coro coro)
            : scheduler_op(&do_complete)
            , h_(coro)
        {
        }
    };

    auto* ph = new post_handler(h);
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0,
            key_posted, reinterpret_cast<LPOVERLAPPED>(ph)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(ph);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_scheduler::
post(scheduler_op* h) const
{
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0,
            key_posted, reinterpret_cast<LPOVERLAPPED>(h)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_scheduler::
on_work_started() noexcept
{
    ::InterlockedIncrement(&outstanding_work_);
}

void
win_scheduler::
on_work_finished() noexcept
{
    ::InterlockedDecrement(&outstanding_work_);
}

bool
win_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
win_scheduler::
work_started() const noexcept
{
    ::InterlockedIncrement(&outstanding_work_);
}

void
win_scheduler::
work_finished() const noexcept
{
    ::InterlockedDecrement(&outstanding_work_);
}

void
win_scheduler::
stop()
{
    if (::InterlockedExchange(&stopped_, 1) == 0)
    {
        if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
        {
            if (!::PostQueuedCompletionStatus(
                iocp_, 0, key_shutdown, nullptr))
            {
                DWORD dwError = ::GetLastError();
                detail::throw_system_error(make_err(dwError));
            }
        }
    }
}

bool
win_scheduler::
stopped() const noexcept
{
    // equivalent to atomic read
    return ::InterlockedExchangeAdd(&stopped_, 0) != 0;
}

void
win_scheduler::
restart()
{
    ::InterlockedExchange(&stopped_, 0);
}

std::size_t
win_scheduler::
run()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(INFINITE))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        // Check if we should exit after processing work
        if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        {
            stop();
            break;
        }
    }
    return n;
}

std::size_t
win_scheduler::
run_one()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(INFINITE);
}

std::size_t
win_scheduler::
wait_one(long usec)
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    unsigned long timeout_ms = usec < 0 ? INFINITE :
        static_cast<unsigned long>((usec + 999) / 1000);
    return do_one(timeout_ms);
}

std::size_t
win_scheduler::
poll()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(0))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

std::size_t
win_scheduler::
poll_one()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(0);
}

void
win_scheduler::
post_deferred_completions(op_queue& ops)
{
    while (auto h = ops.pop())
    {
        if (::PostQueuedCompletionStatus(
                iocp_, 0, key_posted, reinterpret_cast<LPOVERLAPPED>(h)))
            continue;

        // Out of resources, put stuff back
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        completed_ops_.splice(ops);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

std::size_t
win_scheduler::
do_one(unsigned long timeout_ms)
{
    for (;;)
    {
        // Check if we need to process timers or deferred ops
        if (::InterlockedCompareExchange(&dispatch_required_, 0, 1) == 1)
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            post_deferred_completions(completed_ops_);

            if (timer_svc_)
                timer_svc_->process_expired();

            update_timeout();

            // After processing, check if all work is done
            if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
            {
                stop();
                return 0;
            }
        }

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        ::SetLastError(0);

        BOOL result = ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped,
            timeout_ms < max_gqcs_timeout ? timeout_ms : max_gqcs_timeout);
        DWORD dwError = ::GetLastError();

        // Handle based on completion key
        if (overlapped)
        {
            DWORD err = result ? 0 : dwError;

            switch (key)
            {
            case key_io:
            case key_result_stored:
            {
                // Actual I/O completion: overlapped is OVERLAPPED* (first base of overlapped_op)
                auto* ov_op = overlapped_to_op(overlapped);

                // If key_result_stored, results are pre-stored in overlapped fields
                if (key == key_result_stored)
                {
                    bytes = ov_op->bytes_transferred;
                    err = ov_op->dwError;
                }

                ov_op->store_result(bytes, err);
                on_work_finished();
                ov_op->complete(this, bytes, err);
                return 1;
            }

            case key_posted:
            {
                // Posted scheduler_op*: overlapped is actually a scheduler_op*
                auto* op = reinterpret_cast<scheduler_op*>(overlapped);
                on_work_finished();
                op->complete(this, bytes, err);
                return 1;
            }

            default:
                continue;
            }
        }

        // Signal completions (no OVERLAPPED)
        if (result)
        {
            switch (key)
            {
            case key_wake_dispatch:
                // Timer wakeup - loop to check dispatch_required_
                continue;

            case key_shutdown:
                ::InterlockedExchange(&stop_event_posted_, 0);
                if (stopped())
                {
                    // Re-post for other waiting threads
                    if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
                    {
                        ::PostQueuedCompletionStatus(
                            iocp_, 0, key_shutdown, nullptr);
                    }
                    return 0;
                }
                continue;

            default:
                continue;
            }
        }

        // Timeout or error
        if (dwError != WAIT_TIMEOUT)
            detail::throw_system_error(make_err(dwError));
        if (timeout_ms != INFINITE)
            return 0;
    }
}

void
win_scheduler::
on_timer_changed(void* ctx)
{
    static_cast<win_scheduler*>(ctx)->update_timeout();
}

void
win_scheduler::
set_timer_service(timer_service* svc)
{
    timer_svc_ = svc;
    // Pass 'this' as context - callback routes to correct instance
    svc->set_on_earliest_changed(timer_service::callback{this, &on_timer_changed});
    if (timers_)
        timers_->start();
}

void
win_scheduler::
update_timeout()
{
    if (timer_svc_ && timers_)
        timers_->update_timeout(timer_svc_->nearest_expiry());
}

} // namespace boost::corosio::detail

#endif
