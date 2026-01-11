//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/win_iocp_scheduler.hpp"
#include "src/detail/win_overlapped_op.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/thread_local_ptr.hpp>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <Windows.h>

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
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            ops.push_back(completed_ops_);  // splice all from completed_ops_
        }

        if (!ops.empty())
        {
            while (auto* h = ops.pop_front())
            {
                ::InterlockedDecrement(&outstanding_work_);
                h->destroy();
            }
        }
        else
        {
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
                else if (key == socket_key)
                {
                    // I/O operations
                    static_cast<overlapped_op*>(overlapped)->destroy();
                }
            }
        }
    }

    if (timer_thread_.joinable())
        timer_thread_.join();
}

void
win_iocp_scheduler::
post(capy::coro h) const
{
    struct coro_work
        : capy::execution_context::handler  // handler already has intrusive_list node
    {
        capy::coro h_;
        long ready_ = 1;  // always ready for immediate dispatch

        explicit coro_work(capy::coro h)
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

    auto* work = new coro_work(h);
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0, handler_key,
            reinterpret_cast<LPOVERLAPPED>(work)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        completed_ops_.push_back(work);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

void
win_iocp_scheduler::
post(capy::execution_context::handler* h) const
{
    // Mark ready if this is an overlapped_op (safe to dispatch immediately)
    if (auto* op = dynamic_cast<overlapped_op*>(h))
        op->ready_ = 1;

    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(iocp_, 0, handler_key,
            reinterpret_cast<LPOVERLAPPED>(h)))
    {
        // PQCS can fail if non-paged pool exhausted; queue for later
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        completed_ops_.push_back(h);
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
    // Auto-stop when no work remains; run() will return
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
            if (!::PostQueuedCompletionStatus(iocp_, 0, shutdown_key, nullptr))
            {
                DWORD last_error = ::GetLastError();
                detail::throw_system_error(system::error_code(
                    static_cast<int>(last_error), system::system_category()));
            }
        }
    }
}

bool
win_iocp_scheduler::
stopped() const noexcept
{
    // InterlockedExchangeAdd with 0 is an atomic read
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
        return 0;
    if (stopped())
        return 0;

    system::error_code ec;
    std::size_t total = 0;

    while (!stopped())
    {
        std::size_t n = do_run(INFINITE, static_cast<std::size_t>(-1), ec);
        if (ec)
            detail::throw_system_error(ec);
        if (n == 0)
            break;
        total += n;
    }

    return total;
}

std::size_t
win_iocp_scheduler::
run_one()
{
    // Return immediately if stopped
    if (stopped())
        return 0;

    // Return immediately if no work
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        return 0;

    system::error_code ec;
    std::size_t n = do_run(INFINITE, 1, ec);
    if (ec)
        detail::throw_system_error(ec);
    return n;
}

std::size_t
win_iocp_scheduler::
run_one(long usec)
{
    // Return immediately if stopped
    if (stopped())
        return 0;

    // Timed version: wait for timeout even if no work (work could be posted)
    unsigned long timeout_ms = usec < 0 ? INFINITE :
        static_cast<unsigned long>((usec + 999) / 1000);
    system::error_code ec;
    std::size_t n = do_run(timeout_ms, 1, ec);
    if (ec)
        detail::throw_system_error(ec);
    return n;
}

std::size_t
win_iocp_scheduler::
wait_one(long usec)
{
    // Return immediately if stopped
    if (stopped())
        return 0;

    // Timed version: wait for timeout even if no work
    unsigned long timeout_ms = usec < 0 ? INFINITE :
        static_cast<unsigned long>((usec + 999) / 1000);

    system::error_code ec;
    std::size_t n = do_wait(timeout_ms, ec);  // Wait only, don't execute
    if (ec)
        detail::throw_system_error(ec);
    return n;
}

std::size_t
win_iocp_scheduler::
run_for(std::chrono::steady_clock::duration rel_time)
{
    auto end_time = std::chrono::steady_clock::now() + rel_time;
    return run_until(end_time);
}

std::size_t
win_iocp_scheduler::
run_until(std::chrono::steady_clock::time_point abs_time)
{
    // Return immediately if stopped
    if (stopped())
        return 0;

    system::error_code ec;
    std::size_t total = 0;

    while (!stopped())
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= abs_time)
            break;

        // Return if no work
        if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
            break;

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            abs_time - now);
        unsigned long timeout = static_cast<unsigned long>(remaining.count());
        if (timeout == 0)
            timeout = 1;

        std::size_t n = do_run(timeout, static_cast<std::size_t>(-1), ec);
        total += n;

        if (ec)
            detail::throw_system_error(ec);
        if (n == 0)
            break;
    }

    return total;
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

    system::error_code ec;
    std::size_t n = do_run(0, static_cast<std::size_t>(-1), ec);
    if (ec)
        detail::throw_system_error(ec);
    return n;
}

std::size_t
win_iocp_scheduler::
poll_one()
{
    // Return immediately if stopped
    if (stopped())
        return 0;

    // Return immediately if no work
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        return 0;

    system::error_code ec;
    std::size_t n = do_run(0, 1, ec);
    if (ec)
        detail::throw_system_error(ec);
    return n;
}

std::size_t
win_iocp_scheduler::
do_run(
    unsigned long timeout,
    std::size_t max_handlers,
    system::error_code& ec)
{
    std::size_t count = 0;
    thread_context_guard guard(this);
    ec.clear();

    while (count < max_handlers)
    {
        // Drain fallback queue (populated when PQCS fails)
        if (::InterlockedCompareExchange(&dispatch_required_, 0, 1) == 1)
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            while (auto* h = completed_ops_.pop_front())
            {
                ::PostQueuedCompletionStatus(iocp_, 0, handler_key,
                    reinterpret_cast<LPOVERLAPPED>(h));
            }
        }

        // Check if there's any work; if not and we're blocking, return
        if (timeout == INFINITE &&
            ::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        {
            break;
        }

        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;
        ::SetLastError(0);

        // Cap timeout to allow periodic re-checking of conditions
        unsigned long actual_timeout = (timeout == INFINITE || timeout > max_gqcs_timeout)
            ? max_gqcs_timeout : timeout;

        BOOL result = ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped, actual_timeout);
        DWORD last_error = ::GetLastError();

        if (overlapped)
        {
            if (key == handler_key)
            {
                // Handler completions (post, coro) - always ready to dispatch
                // RAII guards for exception safety
                struct work_guard {
                    win_iocp_scheduler* self;
                    ~work_guard() { self->on_work_finished(); }
                } wg{this};

                struct count_guard {
                    std::size_t& n;
                    ~count_guard() { ++n; }
                } cg{count};

                (*reinterpret_cast<capy::execution_context::handler*>(overlapped))();
            }
            else if (key == socket_key)
            {
                auto* op = static_cast<overlapped_op*>(overlapped);

                // Race condition: GQCS can return before WSARecv/etc returns.
                // CAS ready_ from 0->1: if old value was 1, initiator is done
                // and we dispatch. If 0, initiator will see 1 and re-post.
                if (::InterlockedCompareExchange(&op->ready_, 1, 0) == 1)
                {
                    // RAII guards for exception safety
                    struct work_guard {
                        win_iocp_scheduler* self;
                        ~work_guard() { self->on_work_finished(); }
                    } wg{this};

                    struct count_guard {
                        std::size_t& n;
                        ~count_guard() { ++n; }
                    } cg{count};

                    DWORD err = result ? 0 : last_error;
                    op->complete(bytes, err);
                    (*op)();
                }
                // ready_ was 0: initiator still owns the op, will re-post
            }
        }
        else if (!result)
        {
            if (last_error != WAIT_TIMEOUT)
            {
                ec.assign(static_cast<int>(last_error), system::system_category());
                break;
            }
            // Only break on timeout if caller requested non-infinite wait
            if (timeout != INFINITE)
                break;
            // Otherwise continue looping (we used capped timeout)
            continue;
        }
        else if (key == shutdown_key)
        {
            // Clear posted flag; we consumed the event
            ::InterlockedExchange(&stop_event_posted_, 0);

            if (stopped())
            {
                // Cascade wake to next blocked thread
                if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
                    ::PostQueuedCompletionStatus(iocp_, 0, shutdown_key, nullptr);
                break;
            }
        }
    }
    return count;
}

std::size_t
win_iocp_scheduler::
do_wait(unsigned long timeout, system::error_code& ec)
{
    ec.clear();

    if (stopped())
        return 0;

    // Check if there's any work; if not, return
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        return 0;

    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED overlapped;

    BOOL result = ::GetQueuedCompletionStatus(
        iocp_, &bytes, &key, &overlapped, timeout);

    if (!result)
    {
        DWORD err = ::GetLastError();
        if (err == WAIT_TIMEOUT)
            return 0;
        if (overlapped == nullptr)
        {
            ec.assign(static_cast<int>(err), system::system_category());
            return 0;
        }
    }

    if (key == shutdown_key)
    {
        // Clear posted flag; we consumed the event
        ::InterlockedExchange(&stop_event_posted_, 0);

        if (stopped())
        {
            // Cascade wake to next blocked thread
            if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
                ::PostQueuedCompletionStatus(iocp_, 0, shutdown_key, nullptr);
        }
        return 0;
    }

    // Re-post without executing - wait_one just checks for availability
    if (overlapped != nullptr && (key == handler_key || key == socket_key))
    {
        ::PostQueuedCompletionStatus(iocp_, bytes, key, overlapped);
        return 1;
    }

    return 0;
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32
