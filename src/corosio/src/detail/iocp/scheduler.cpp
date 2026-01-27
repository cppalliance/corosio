//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/iocp/overlapped_op.hpp"
#include "src/detail/iocp/timers.hpp"
#include "src/detail/timer_service.hpp"
#include "src/detail/iocp/resolver_service.hpp"
#include "src/detail/make_err.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <limits>

/*
    ARCHITECTURE NOTE: Polymorphic Completion Keys

    Each subsystem owns a completion_key-derived object whose address serves
    as the IOCP completion key. When GQCS returns, we cast the key back to
    completion_key* and dispatch polymorphically via on_completion().

    Key ownership:
      - win_scheduler owns handler_key_ and shutdown_key_
      - win_sockets owns overlapped_key_
      - win_timers IS a completion_key (derives from it)

    The op_queue (intrusive_list<scheduler_op>) holds MIXED elements:
      - Plain handlers (coro_work, etc.)
      - overlapped_op (which derives from scheduler_op)

    Use get_overlapped_op(scheduler_op*) to safely check if a scheduler_op is an
    overlapped_op (returns nullptr if not). All code that processes op_queue
    must be mindful of this mixed content.
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

// used for running_in_this_thread()
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

completion_key::result
win_scheduler::handler_key::
on_completion(
    win_scheduler& sched,
    DWORD,
    DWORD,
    LPOVERLAPPED overlapped)
{
    struct work_guard
    {
        win_scheduler* self;
        ~work_guard() { self->on_work_finished(); }
    };

    work_guard g{&sched};
    (*reinterpret_cast<scheduler_op*>(overlapped))();
    return result::did_work;
}

void
win_scheduler::handler_key::
destroy(LPOVERLAPPED overlapped)
{
    reinterpret_cast<scheduler_op*>(overlapped)->destroy();
}

completion_key::result
win_scheduler::shutdown_key::
on_completion(
    win_scheduler& sched,
    DWORD,
    DWORD,
    LPOVERLAPPED)
{
    ::InterlockedExchange(&sched.stop_event_posted_, 0);
    if (sched.stopped())
    {
        if (::InterlockedExchange(&sched.stop_event_posted_, 1) == 0)
            repost(sched.iocp_);
        return result::stop_loop;
    }
    return result::continue_loop;
}

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
        // First drain the fallback queue (intrusive_list doesn't auto-destroy)
        op_queue ops;
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            ops.splice(completed_ops_);  // splice all from completed_ops_
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
        if (overlapped && key != 0)
        {
            ::InterlockedDecrement(&outstanding_work_);
            reinterpret_cast<completion_key*>(key)->destroy(overlapped);
        }
    }
}

void
win_scheduler::
post(capy::coro h) const
{
    struct post_handler final
        : scheduler_op
    {
        capy::coro h_;
        long ready_ = 1;  // always ready for immediate dispatch

        explicit
        post_handler(capy::coro h)
            : h_(h)
        {
        }

        ~post_handler() = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
            h.resume();
        }

        void destroy() override
        {
            delete this;
        }
    };

    auto* ph = new post_handler(h);
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0,
            reinterpret_cast<ULONG_PTR>(&handler_key_),
            reinterpret_cast<LPOVERLAPPED>(ph)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(ph);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_scheduler::
post(scheduler_op* h) const
{
    // Mark ready if this is an overlapped_op (safe to dispatch immediately)
    if (auto* op = get_overlapped_op(h))
        op->ready_ = 1;

    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0,
            reinterpret_cast<ULONG_PTR>(&handler_key_),
            reinterpret_cast<LPOVERLAPPED>(h)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
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
    if (::InterlockedDecrement(&outstanding_work_) == 0)
        stop();
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
    // Only act on first stop() call
    if (::InterlockedExchange(&stopped_, 1) == 0)
    {
        // PQCS consumes non-paged pool memory; avoid exhaustion by
        // limiting to one outstanding stop event across all threads
        if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
        {
            if (!::PostQueuedCompletionStatus(
                iocp_, 0,
                reinterpret_cast<ULONG_PTR>(&shutdown_key_),
                nullptr))
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
    while (do_one(INFINITE))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
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
post_deferred_completions(
    op_queue& ops)
{
    while(auto h = ops.pop())
    {
        // Mark ready for overlapped_ops
        if(auto op = get_overlapped_op(h))
            op->ready_ = 1;

        if(::PostQueuedCompletionStatus(
                iocp_, 0,
                reinterpret_cast<ULONG_PTR>(&handler_key_),
                reinterpret_cast<LPOVERLAPPED>(h)))
            continue;

        // Out of resources again, put stuff back
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
        if (::InterlockedCompareExchange(&dispatch_required_, 0, 1) == 1)
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            post_deferred_completions(completed_ops_);

            if (timer_svc_)
                timer_svc_->process_expired();

            update_timeout();
        }

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        ::SetLastError(0);

        BOOL result = ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped,
            timeout_ms < max_gqcs_timeout ? timeout_ms : max_gqcs_timeout);
        DWORD dwError = ::GetLastError();

        if (overlapped || (result && key != 0))
        {
            auto* target = reinterpret_cast<completion_key*>(key);
            DWORD dwErr = result ? 0 : dwError;
            auto r = target->on_completion(*this, bytes, dwErr, overlapped);

            if (r == completion_key::result::did_work)
                return 1;
            if (r == completion_key::result::stop_loop)
                return 0;
            continue;
        }

        if (!result)
        {
            if (dwError != WAIT_TIMEOUT)
                detail::throw_system_error(make_err(dwError));
            if (timeout_ms != INFINITE)
                return 0;
        }
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
