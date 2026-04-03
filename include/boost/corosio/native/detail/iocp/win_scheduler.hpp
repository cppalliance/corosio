//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/detail/scheduler.hpp>
#include <system_error>

#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>

#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_timers.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/iocp/win_resolver_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

// Forward declarations
struct overlapped_op;
class win_timers;

class BOOST_COROSIO_DECL win_scheduler final
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    win_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);
    ~win_scheduler();
    win_scheduler(win_scheduler const&)            = delete;
    win_scheduler& operator=(win_scheduler const&) = delete;

    void shutdown() override;
    void post(std::coroutine_handle<> h) const override;
    void post(scheduler_op* h) const override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

    void* native_handle() const noexcept
    {
        return iocp_;
    }

    void work_started() noexcept override;
    void work_finished() noexcept override;

    /** Apply runtime IOCP configuration.

        @param gqcs_timeout_ms  Max GQCS blocking time in milliseconds.
    */
    void configure_iocp(unsigned gqcs_timeout_ms) noexcept
    {
        gqcs_timeout_ms_ = gqcs_timeout_ms;
    }

    /** Enable or disable single-threaded (lockless) mode.

        When enabled, the dispatch mutex becomes a no-op.
        Cross-thread post() is undefined behavior.
    */
    void configure_single_threaded(bool v) noexcept
    {
        single_threaded_ = v;
        dispatch_mutex_.set_enabled(!v);
    }

    /** Signal that an overlapped I/O operation is now pending.
        Coordinates with do_one() via the ready_ CAS protocol. */
    void on_pending(overlapped_op* op) const;

    /** Post an immediate completion with pre-stored results.
        Used for sync errors and noop paths. */
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) const;

    // Timer service integration
    void set_timer_service(timer_service* svc);
    void update_timeout();

private:
    static void on_timer_changed(void* ctx);
    void post_deferred_completions(op_queue& ops);
    std::size_t do_one(unsigned long timeout_ms);

    timer_service* timer_svc_ = nullptr;
    void* iocp_;
    mutable long outstanding_work_;
    mutable long stopped_;
    long stop_event_posted_;
    mutable long dispatch_required_;
    unsigned long gqcs_timeout_ms_ = 500;
    bool single_threaded_ = false;

    mutable win_mutex dispatch_mutex_;
    mutable op_queue completed_ops_;
    std::unique_ptr<win_timers> timers_;
};

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

namespace iocp {

// Max timeout for GQCS to allow periodic re-checking of conditions.
// Matches Asio's default_gqcs_timeout for pre-Vista compatibility.
inline constexpr unsigned long max_gqcs_timeout = 500;

struct BOOST_COROSIO_SYMBOL_VISIBLE scheduler_context
{
    win_scheduler const* key;
    scheduler_context* next;
};

inline thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(win_scheduler const* ctx) noexcept
        : frame_{ctx, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        context_stack.set(frame_.next);
    }
};

} // namespace iocp

inline win_scheduler::win_scheduler(
    capy::execution_context& ctx, int concurrency_hint)
    : iocp_(nullptr)
    , outstanding_work_(0)
    , stopped_(0)
    , stop_event_posted_(0)
    , dispatch_required_(0)
{
    // concurrency_hint < 0 means use system default (DWORD(~0) = max)
    iocp_ = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0,
        static_cast<DWORD>(
            concurrency_hint >= 0 ? concurrency_hint : DWORD(~0)));

    if (iocp_ == nullptr)
        detail::throw_system_error(make_err(::GetLastError()));

    // Create timer wakeup mechanism (tries NT native, falls back to thread)
    timers_ = make_win_timers(iocp_, &dispatch_required_);

    // Connect timer service to scheduler
    set_timer_service(&get_timer_service(ctx, *this));

    // Initialize resolver service
    ctx.make_service<win_resolver_service>(*this);
}

inline win_scheduler::~win_scheduler()
{
    if (iocp_ != nullptr)
        ::CloseHandle(iocp_);
}

inline void
win_scheduler::shutdown()
{
    if (timers_)
        timers_->stop();

    // Drain timer heap before the work-counting loop. The timer_service
    // was registered after this scheduler (nested make_service from our
    // constructor), so execution_context::shutdown() calls us first.
    // Asio avoids this by owning timer queues directly inside the
    // scheduler; we bridge the gap by shutting down the timer service
    // early. The subsequent call from execution_context is a no-op.
    if (timer_svc_)
        timer_svc_->shutdown();

    while (::InterlockedExchangeAdd(&outstanding_work_, 0) > 0)
    {
        op_queue ops;
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            ops.splice(completed_ops_);
        }

        if (!ops.empty())
        {
            while (auto* h = ops.pop())
            {
                ::InterlockedDecrement(&outstanding_work_);
                h->destroy();
            }
        }
        else
        {
            DWORD bytes;
            ULONG_PTR key;
            LPOVERLAPPED overlapped;
            ::GetQueuedCompletionStatus(
                iocp_, &bytes, &key, &overlapped, gqcs_timeout_ms_);
            if (overlapped)
            {
                ::InterlockedDecrement(&outstanding_work_);
                if (key == key_posted)
                {
                    auto* op = reinterpret_cast<scheduler_op*>(overlapped);
                    op->destroy();
                }
                else
                {
                    auto* op = overlapped_to_op(overlapped);
                    op->destroy();
                }
            }
        }
    }
}

inline void
win_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;

        static void do_complete(
            void* owner, scheduler_op* base, std::uint32_t, std::uint32_t)
        {
            auto* self = static_cast<post_handler*>(base);
            if (!owner)
            {
                // Shutdown path: destroy the coroutine frame synchronously.
                //
                // Bounded destruction invariant: the chain triggered by
                // coro.destroy() is at most two levels deep:
                //   1. task frame destroyed → ~io_awaitable_promise_base()
                //      destroys stored continuation (if != noop_coroutine)
                //   2. continuation (trampoline) destroyed → final_suspend
                //      returns suspend_never, no further continuation
                //
                // If a future refactor adds deeper continuation chains,
                // this would reintroduce re-entrant stack overflow risk.
#ifndef NDEBUG
                static thread_local int destroy_depth = 0;
                ++destroy_depth;
                BOOST_COROSIO_ASSERT(destroy_depth <= 2);
#endif
                auto coro = self->h_;
                delete self;
                coro.destroy();
#ifndef NDEBUG
                --destroy_depth;
#endif
                return;
            }
            auto coro = self->h_;
            delete self;
            std::atomic_thread_fence(std::memory_order_acquire);
            coro.resume();
        }

        explicit post_handler(std::coroutine_handle<> coro)
            : scheduler_op(&do_complete)
            , h_(coro)
        {
        }
    };

    auto* ph = new post_handler(h);
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(
            iocp_, 0, key_posted, reinterpret_cast<LPOVERLAPPED>(ph)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(ph);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

inline void
win_scheduler::post(scheduler_op* h) const
{
    ::InterlockedIncrement(&outstanding_work_);

    if (!::PostQueuedCompletionStatus(
            iocp_, 0, key_posted, reinterpret_cast<LPOVERLAPPED>(h)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

inline bool
win_scheduler::running_in_this_thread() const noexcept
{
    for (auto* c = iocp::context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

inline void
win_scheduler::work_started() noexcept
{
    ::InterlockedIncrement(&outstanding_work_);
}

inline void
win_scheduler::work_finished() noexcept
{
    if (::InterlockedDecrement(&outstanding_work_) == 0)
        stop();
}

inline void
win_scheduler::on_pending(overlapped_op* op) const
{
    // CAS: try to set ready_ from 0 to 1.
    // If the old value was 1, GQCS already grabbed this op and stored
    // results — we need to re-post so do_one() can dispatch it.
    if (::InterlockedCompareExchange(&op->ready_, 1, 0) == 1)
    {
        if (!::PostQueuedCompletionStatus(
                iocp_, 0, key_result_stored, static_cast<LPOVERLAPPED>(op)))
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            completed_ops_.push(op);
            ::InterlockedExchange(&dispatch_required_, 1);
        }
    }
}

inline void
win_scheduler::on_completion(overlapped_op* op, DWORD error, DWORD bytes) const
{
    // Sync completion: pack results into op and post for dispatch.
    op->ready_            = 1;
    op->dwError           = error;
    op->bytes_transferred = bytes;

    if (!::PostQueuedCompletionStatus(
            iocp_, 0, key_result_stored, static_cast<LPOVERLAPPED>(op)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(op);
        ::InterlockedExchange(&dispatch_required_, 1);
    }
}

inline void
win_scheduler::stop()
{
    if (::InterlockedExchange(&stopped_, 1) == 0)
    {
        if (::InterlockedExchange(&stop_event_posted_, 1) == 0)
        {
            if (!::PostQueuedCompletionStatus(iocp_, 0, key_shutdown, nullptr))
            {
                // PQCS failure is non-fatal: stopped_ is already set.
                // The run() loop will notice via the GQCS timeout
                // (gqcs_timeout_ms_, default 500ms) and exit.
                ::InterlockedExchange(&dispatch_required_, 1);
            }
        }
    }
}

inline bool
win_scheduler::stopped() const noexcept
{
    // equivalent to atomic read
    return ::InterlockedExchangeAdd(&stopped_, 0) != 0;
}

inline void
win_scheduler::restart()
{
    ::InterlockedExchange(&stopped_, 0);
    ::InterlockedExchange(&stop_event_posted_, 0);
}

inline std::size_t
win_scheduler::run()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    iocp::thread_context_guard ctx(this);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(INFINITE))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
        {
            stop();
            break;
        }
    }
    return n;
}

inline std::size_t
win_scheduler::run_one()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    iocp::thread_context_guard ctx(this);
    return do_one(INFINITE);
}

inline std::size_t
win_scheduler::wait_one(long usec)
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    iocp::thread_context_guard ctx(this);
    unsigned long timeout_ms = INFINITE;
    if (usec >= 0)
    {
        auto ms    = (static_cast<long long>(usec) + 999) / 1000;
        timeout_ms = ms >= 0xFFFFFFFELL ? static_cast<unsigned long>(0xFFFFFFFE)
                                        : static_cast<unsigned long>(ms);
    }
    return do_one(timeout_ms);
}

inline std::size_t
win_scheduler::poll()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    iocp::thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(0))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

inline std::size_t
win_scheduler::poll_one()
{
    if (::InterlockedExchangeAdd(&outstanding_work_, 0) == 0)
    {
        stop();
        return 0;
    }

    iocp::thread_context_guard ctx(this);
    return do_one(0);
}

inline void
win_scheduler::post_deferred_completions(op_queue& ops)
{
    while (auto h = ops.pop())
    {
        if (::PostQueuedCompletionStatus(
                iocp_, 0, key_posted, reinterpret_cast<LPOVERLAPPED>(h)))
            continue;

        // Out of resources, put the failed op and remaining ops back
        ops.push(h);
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.splice(ops);
        ::InterlockedExchange(&dispatch_required_, 1);
        return;
    }
}

inline std::size_t
win_scheduler::do_one(unsigned long timeout_ms)
{
    for (;;)
    {
        // Check if we need to process timers or deferred ops
        if (::InterlockedCompareExchange(&dispatch_required_, 0, 1) == 1)
        {
            op_queue local_ops;
            {
                std::lock_guard<win_mutex> lock(dispatch_mutex_);
                local_ops.splice(completed_ops_);
            }
            post_deferred_completions(local_ops);

            if (timer_svc_)
                timer_svc_->process_expired();

            update_timeout();
        }

        DWORD bytes             = 0;
        ULONG_PTR key           = 0;
        LPOVERLAPPED overlapped = nullptr;
        ::SetLastError(0);

        BOOL result = ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped,
            timeout_ms < gqcs_timeout_ms_ ? timeout_ms
                                        : gqcs_timeout_ms_);
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
                auto* ov_op = overlapped_to_op(overlapped);

                // If key_result_stored, results are pre-stored in op fields
                if (key == key_result_stored)
                {
                    bytes = ov_op->bytes_transferred;
                    err   = ov_op->dwError;
                }

                // Store GQCS results so on_pending() re-post has valid data
                ov_op->store_result(bytes, err);

                // CAS: try to set ready_ from 0 to 1.
                // If old value was 1, the initiator already returned
                // (on_pending/on_completion set it) — safe to dispatch.
                // If old value was 0, the initiator hasn't returned yet —
                // skip dispatch; on_pending() will re-post.
                if (::InterlockedCompareExchange(&ov_op->ready_, 1, 0) == 1)
                {
                    ov_op->complete(this, bytes, err);
                    work_finished();
                    return 1;
                }
                continue;
            }

            case key_posted:
            {
                // Posted scheduler_op*: overlapped is actually a scheduler_op*
                auto* op = reinterpret_cast<scheduler_op*>(overlapped);
                op->complete(this, bytes, err);
                work_finished();
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
        // PQCS-failure fallback: stop() sets stopped_ and
        // dispatch_required_ but if the key_shutdown post failed,
        // no completion is ever dequeued.  Catch it here on the
        // periodic 500 ms GQCS timeout so run()/run_one() can exit.
        if (stopped())
            return 0;
    }
}

inline void
win_scheduler::on_timer_changed(void* ctx)
{
    static_cast<win_scheduler*>(ctx)->update_timeout();
}

inline void
win_scheduler::set_timer_service(timer_service* svc)
{
    timer_svc_ = svc;
    // Pass 'this' as context - callback routes to correct instance
    svc->set_on_earliest_changed(
        timer_service::callback{this, &on_timer_changed});
    if (timers_)
        timers_->start();
}

inline void
win_scheduler::update_timeout()
{
    if (timer_svc_ && timers_)
        timers_->update_timeout(timer_svc_->nearest_expiry());
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP
