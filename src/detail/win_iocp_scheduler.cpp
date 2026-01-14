//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifdef _WIN32

#include "detail/win_iocp_scheduler.hpp"
#include "detail/win_overlapped_op.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/core/thread_local_ptr.hpp>

#include <limits>

/*
    ARCHITECTURE NOTE: Difference from Boost.Asio

    In Asio, only OVERLAPPED-derived operations go through the completion port.
    Posted handlers are wrapped in an operation that contains OVERLAPPED.

    In corosio, we post BOTH types directly to the completion port:
      - OVERLAPPED* (overlapped_op) for I/O operations
      - capy::execution_context::handler* for posted handlers/coroutines

    Discrimination is done via the completion key:
      - handler_key (1): the LPOVERLAPPED is actually a handler*
      - overlapped_key (2): the LPOVERLAPPED is an overlapped_op*

    The op_queue (intrusive_list<handler>) holds MIXED elements:
      - Plain handlers (coro_work, etc.)
      - overlapped_op (which derives from handler)

    Use get_overlapped_op(handler*) to safely check if a handler is an
    overlapped_op (returns nullptr if not). All code that processes op_queue
    must be mindful of this mixed content.
*/

namespace boost {
namespace corosio {
namespace detail {

namespace {

// Max timeout for GQCS to allow periodic re-checking of conditions
constexpr unsigned long max_gqcs_timeout = 500;

inline
system::error_code
last_error() noexcept
{
    return system::error_code(
        static_cast<int>(GetLastError()),
        system::system_category());
}

struct scheduler_context
{
    win_iocp_scheduler const* key;
    scheduler_context* next;
};

// used for running_in_this_thread()
capy::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        win_iocp_scheduler const* ctx) noexcept
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

win_iocp_scheduler::
win_iocp_scheduler(
    capy::execution_context&,
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
        detail::throw_system_error(last_error());
}

win_iocp_scheduler::
~win_iocp_scheduler()
{
    if (iocp_ != nullptr)
        ::CloseHandle(iocp_);
}

void
win_iocp_scheduler::
shutdown()
{
    ::InterlockedExchange(&shutdown_, 1);

    // TODO: Signal timer thread when timer support is added

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
        if (overlapped)
        {
            ::InterlockedDecrement(&outstanding_work_);
            if (key == handler_key)
            {
                // Posted handlers (coro_work, etc.)
                reinterpret_cast<capy::execution_context::handler*>(overlapped)->destroy();
            }
            else if (key == overlapped_key)
            {
                // I/O operations
                static_cast<overlapped_op*>(overlapped)->destroy();
            }
        }
    }

    if (timer_thread_.joinable())
        timer_thread_.join();
}

void
win_iocp_scheduler::
post(capy::any_coro h) const
{
    struct post_handler
        : capy::execution_context::handler
    {
        capy::any_coro h_;
        long ready_ = 1;  // always ready for immediate dispatch

        explicit
        post_handler(capy::any_coro h)
            : h_(h)
        {
        }

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

    if (!::PostQueuedCompletionStatus(iocp_, 0, handler_key,
            reinterpret_cast<LPOVERLAPPED>(ph)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(ph);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_iocp_scheduler::
post(capy::execution_context::handler* h) const
{
    // Mark ready if this is an overlapped_op (safe to dispatch immediately)
    if (auto* op = get_overlapped_op(h))
        op->ready_ = 1;

    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0, handler_key,
            reinterpret_cast<LPOVERLAPPED>(h)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_iocp_scheduler::
on_work_started() noexcept
{
    ::InterlockedIncrement(&outstanding_work_);
}

void
win_iocp_scheduler::
on_work_finished() noexcept
{
    if (::InterlockedDecrement(&outstanding_work_) == 0)
        stop();
}

bool
win_iocp_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
win_iocp_scheduler::
work_started() const noexcept
{
    ::InterlockedIncrement(&outstanding_work_);
}

void
win_iocp_scheduler::
work_finished() const noexcept
{
    ::InterlockedDecrement(&outstanding_work_);
}

void
win_iocp_scheduler::
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
                iocp_, 0, shutdown_key, nullptr))
            {
                DWORD last_error = ::GetLastError();
                detail::throw_system_error(system::error_code(
                    static_cast<int>(last_error),
                    system::system_category()));
            }
        }
    }
}

bool
win_iocp_scheduler::
stopped() const noexcept
{
    // equivalent to atomic read
    return ::InterlockedExchangeAdd(&stopped_, 0) != 0;
}

void
win_iocp_scheduler::
restart()
{
    ::InterlockedExchange(&stopped_, 0);
}

std::size_t
win_iocp_scheduler::
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
win_iocp_scheduler::
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
win_iocp_scheduler::
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
win_iocp_scheduler::
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
win_iocp_scheduler::
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
win_iocp_scheduler::
post_deferred_completions(
    op_queue& ops)
{
    while(auto h = ops.pop())
    {
        if(auto op = get_overlapped_op(h))
        {
            op->ready_ = 1;
            if(::PostQueuedCompletionStatus(
                    iocp_, 0, overlapped_key, op))
                continue;
        }
        else
        {
            if(::PostQueuedCompletionStatus(
                iocp_, 0, handler_key,
                    reinterpret_cast<LPOVERLAPPED>(h)))
                continue;
        }

        // out of resources again, put stuff back
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        completed_ops_.splice(ops);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

// RAII guard - work_finished called even if handler throws
struct work_guard
{
    win_iocp_scheduler* self;
    ~work_guard() { self->on_work_finished(); }
};

// Execute exactly ONE handler, return 0 or 1
// THROWS on system errors - no error_code
std::size_t
win_iocp_scheduler::
do_one(unsigned long timeout_ms)
{
    for (;;)
    {
        // Drain fallback queue if needed
        if (::InterlockedCompareExchange(&dispatch_required_, 0, 1) == 1)
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            post_deferred_completions(completed_ops_);
        }

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        ::SetLastError(0);

        BOOL result = ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped,
            timeout_ms < max_gqcs_timeout ? timeout_ms : max_gqcs_timeout);
        DWORD last_error = ::GetLastError();

        if (overlapped)
        {
            if (key == handler_key)
            {
                // handler*
                work_guard g{this};
                (*reinterpret_cast<capy::execution_context::handler*>(overlapped))();
                return 1;
            }
            else if (key == overlapped_key)
            {
                // overlapped_op*
                auto* op = static_cast<overlapped_op*>(overlapped);
                if (::InterlockedCompareExchange(&op->ready_, 1, 0) == 0)
                {
                    work_guard g{this};
                    DWORD err = result ? 0 : last_error;
                    op->complete(bytes, err);
                    (*op)();
                    return 1;
                }
                // Not ready - loop to get next completion
            }
        }
        else if (!result)
        {
            if (last_error != WAIT_TIMEOUT)
            {
                // THROW directly - no error_code
                detail::throw_system_error(system::error_code(
                    static_cast<int>(last_error), system::system_category()));
            }
            // Timeout - break if finite, continue if INFINITE (capped timeout)
            if (timeout_ms != INFINITE)
                return 0;
        }
        else if (key == shutdown_key)
        {
            ::InterlockedExchange(&stop_event_posted_, 0);
            if (stopped())
            {
                if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
                    ::PostQueuedCompletionStatus(iocp_, 0, shutdown_key, nullptr);
                return 0;
            }
            // Not stopped (restarted) - continue immediately without
            // checking timeout, since shutdown_key doesn't count as work
            continue;
        }
    }
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
