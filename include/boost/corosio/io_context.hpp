//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_CONTEXT_HPP
#define BOOST_COROSIO_IO_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/continuation_op.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <thread>

namespace boost::corosio {

/** Runtime tuning options for @ref io_context.

    All fields have defaults that match the library's built-in
    values, so constructing a default `io_context_options` produces
    identical behavior to an unconfigured context.

    Options that apply only to a specific backend family are
    silently ignored when the active backend does not support them.

    @par Example
    @code
    io_context_options opts;
    opts.max_events_per_poll  = 256;   // larger batch per syscall
    opts.inline_budget_max    = 32;    // more speculative completions
    opts.thread_pool_size     = 4;     // more file-I/O workers

    io_context ioc(opts);
    @endcode

    @see io_context, native_io_context
*/
struct io_context_options
{
    /** Maximum events fetched per reactor poll call.

        Controls the buffer size passed to `epoll_wait()` or
        `kevent()`. Larger values reduce syscall frequency under
        high load; smaller values improve fairness between
        connections. Ignored on IOCP and select backends.
    */
    unsigned max_events_per_poll = 128;

    /** Starting inline completion budget per handler chain.

        After a posted handler executes, the reactor grants this
        many speculative inline completions before forcing a
        re-queue. Applies to reactor backends only.

        @note Constructing an `io_context` with `concurrency_hint > 1`
            and all three budget fields at their defaults overrides
            them to disable inline completion (post-everything mode),
            since multi-thread workloads benefit from cross-thread
            work-stealing. Setting any budget field to a non-default
            value disables the override.
    */
    unsigned inline_budget_initial = 2;

    /** Hard ceiling on adaptive inline budget ramp-up.

        The budget doubles each cycle it is fully consumed, up to
        this limit. Applies to reactor backends only.
    */
    unsigned inline_budget_max = 16;

    /** Inline budget when no other thread assists the reactor.

        When only one thread is running the event loop, this
        value caps the inline budget to preserve fairness.
        Applies to reactor backends only.
    */
    unsigned unassisted_budget = 4;

    /** Maximum `GetQueuedCompletionStatus` timeout in milliseconds.

        Bounds how long the IOCP scheduler blocks between timer
        rechecks. Lower values improve timer responsiveness at the
        cost of more syscalls. Applies to IOCP only.
    */
    unsigned gqcs_timeout_ms = 500;

    /** Thread pool size for blocking I/O (file I/O, DNS resolution).

        Sets the number of worker threads in the shared thread pool
        used by POSIX file services and DNS resolution. Must be at
        least 1. Applies to POSIX backends only; ignored on IOCP
        where file I/O uses native overlapped I/O.
    */
    unsigned thread_pool_size = 1;

    /** Enable single-threaded mode (disable scheduler locking).

        When true, the scheduler skips all mutex lock/unlock and
        condition variable operations on the hot path. This
        eliminates synchronization overhead when only one thread
        calls `run()`.

        @par Restrictions
        - Only one thread may call `run()` (or any run variant).
        - Posting work from another thread is undefined behavior.
        - DNS resolution returns `operation_not_supported`.
        - POSIX file I/O returns `operation_not_supported`.
        - Signal sets should not be shared across contexts.

        @note Constructing an `io_context` with `concurrency_hint == 1`
            automatically enables single-threaded mode regardless of
            this field's value, matching asio's convention. To opt out,
            pass `concurrency_hint > 1`.
    */
    bool single_threaded = false;

    /** Enable IORING_SETUP_SQPOLL on the io_uring backend.

        With SQPOLL, the kernel forks a thread that busy-polls the
        submission ring; submission becomes a userspace-only memory
        store, eliminating the io_uring_enter syscall on the submit
        path. Most useful for sustained traffic. Idle thread parks
        after `sq_thread_idle_ms` of no activity.

        Independent of `single_threaded`. Default: off.

        Ignored on non-io_uring backends.
    */
    bool enable_sqpoll = false;

    /** SQ-poll idle timeout in milliseconds.

        After this many ms of no submissions, the kernel polling
        thread sleeps; next submit re-wakes it via SQ_WAKEUP. 0
        means use the kernel default (1ms). Recommended for bursty
        workloads: 100-1000ms (avoids park/unpark thrash).

        Ignored unless `enable_sqpoll` is true. Ignored on
        non-io_uring backends.
    */
    unsigned sq_thread_idle_ms = 0;

    /** Pin the SQ-poll kernel thread to this CPU.

        -1 means do not pin (kernel scheduler picks). Pinning off
        the dispatch core is recommended on latency-sensitive
        deployments to avoid cache contention.

        Ignored unless `enable_sqpoll` is true. Ignored on
        non-io_uring backends.
    */
    int sq_thread_cpu = -1;
};

namespace detail {
class timer_service;
} // namespace detail

/** An I/O context for running asynchronous operations.

    The io_context provides an execution environment for async
    operations. It maintains a queue of pending work items and
    processes them when `run()` is called.

    The default and unsigned constructors select the platform's
    native backend:
    - Windows: IOCP
    - Linux: epoll
    - BSD/macOS: kqueue
    - Other POSIX: select

    The template constructor accepts a backend tag value to
    choose a specific backend at compile time:

    @par Example
    @code
    io_context ioc;                   // platform default
    io_context ioc2(corosio::epoll);  // explicit backend
    @endcode

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, if using a concurrency hint greater
    than 1.

    @see epoll_t, select_t, kqueue_t, iocp_t
*/
class BOOST_COROSIO_DECL io_context : public capy::execution_context
{
    /// Pre-create services that depend on options (before construct).
    void apply_options_pre_(io_context_options const& opts);

    /// Apply runtime tuning to the scheduler (after construct).
    void apply_options_post_(
        io_context_options const& opts,
        unsigned concurrency_hint);

    /// Switch the scheduler to single-threaded (lockless) mode.
    void configure_single_threaded_();

protected:
    detail::scheduler* sched_;

public:
    /** The executor type for this context. */
    class executor_type;

    /** Construct with default concurrency and platform backend.

        Uses `std::thread::hardware_concurrency()` clamped to a minimum
        of 2 as the concurrency hint, so the default constructor never
        silently engages single-threaded mode (see
        @ref io_context_options::single_threaded). Pass an explicit
        `concurrency_hint == 1` to opt into single-threaded mode.
    */
    io_context();

    /** Construct with a concurrency hint and platform backend.

        @param concurrency_hint Hint for the number of threads
            that will call `run()`.
    */
    explicit io_context(unsigned concurrency_hint);

    /** Construct with runtime tuning options and platform backend.

        @param opts Runtime options controlling scheduler and
            service behavior.
        @param concurrency_hint Hint for the number of threads
            that will call `run()`.
    */
    explicit io_context(
        io_context_options const& opts,
        unsigned concurrency_hint = std::thread::hardware_concurrency());

    /** Construct with an explicit backend tag.

        @param backend The backend tag value selecting the I/O
            multiplexer (e.g. `corosio::epoll`).
        @param concurrency_hint Hint for the number of threads
            that will call `run()`.
    */
    template<class Backend>
        requires requires { Backend::construct; }
    explicit io_context(
        Backend backend,
        unsigned concurrency_hint = std::thread::hardware_concurrency())
        : capy::execution_context(this)
        , sched_(nullptr)
    {
        (void)backend;
        sched_ = &Backend::construct(*this, concurrency_hint);
        if (concurrency_hint == 1)
            configure_single_threaded_();
    }

    /** Construct with an explicit backend tag and runtime options.

        @param backend The backend tag value selecting the I/O
            multiplexer (e.g. `corosio::epoll`).
        @param opts Runtime options controlling scheduler and
            service behavior.
        @param concurrency_hint Hint for the number of threads
            that will call `run()`.
    */
    template<class Backend>
        requires requires { Backend::construct; }
    explicit io_context(
        Backend backend,
        io_context_options const& opts,
        unsigned concurrency_hint = std::thread::hardware_concurrency())
        : capy::execution_context(this)
        , sched_(nullptr)
    {
        (void)backend;
        apply_options_pre_(opts);
        sched_ = &Backend::construct(*this, concurrency_hint);
        apply_options_post_(opts, concurrency_hint);
    }

    ~io_context();

    io_context(io_context const&)            = delete;
    io_context& operator=(io_context const&) = delete;

    /** Return an executor for this context.

        The returned executor can be used to dispatch coroutines
        and post work items to this context.

        @return An executor associated with this context.
    */
    executor_type get_executor() const noexcept;

    /** Signal the context to stop processing.

        This causes `run()` to return as soon as possible. Any pending
        work items remain queued.
    */
    void stop()
    {
        sched_->stop();
    }

    /** Return whether the context has been stopped.

        @return `true` if `stop()` has been called and `restart()`
            has not been called since.
    */
    bool stopped() const noexcept
    {
        return sched_->stopped();
    }

    /** Restart the context after being stopped.

        This function must be called before `run()` can be called
        again after `stop()` has been called.
    */
    void restart()
    {
        sched_->restart();
    }

    /** Process all pending work items.

        This function blocks until all pending work items have been
        executed or `stop()` is called. The context is stopped
        when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed.
    */
    std::size_t run()
    {
        return sched_->run();
    }

    /** Process at most one pending work item.

        This function blocks until one work item has been executed
        or `stop()` is called. The context is stopped when there
        is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t run_one()
    {
        return sched_->run_one();
    }

    /** Process work items for the specified duration.

        This function blocks until work items have been executed for
        the specified duration, or `stop()` is called. The context
        is stopped when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @param rel_time The duration for which to process work.

        @return The number of handlers executed.
    */
    template<class Rep, class Period>
    std::size_t run_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process work items until the specified time.

        This function blocks until the specified time is reached
        or `stop()` is called. The context is stopped when there
        is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @param abs_time The time point until which to process work.

        @return The number of handlers executed.
    */
    template<class Clock, class Duration>
    std::size_t
    run_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        std::size_t n = 0;
        while (run_one_until(abs_time))
            if (n != (std::numeric_limits<std::size_t>::max)())
                ++n;
        return n;
    }

    /** Process at most one work item for the specified duration.

        This function blocks until one work item has been executed,
        the specified duration has elapsed, or `stop()` is called.
        The context is stopped when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @param rel_time The duration for which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Rep, class Period>
    std::size_t run_one_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_one_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process at most one work item until the specified time.

        This function blocks until one work item has been executed,
        the specified time is reached, or `stop()` is called.
        The context is stopped when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @param abs_time The time point until which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Clock, class Duration>
    std::size_t
    run_one_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        typename Clock::time_point now = Clock::now();
        for (;;)
        {
            auto rel_time = abs_time - now;
            using rel_type = decltype(rel_time);
            if (rel_time < rel_type::zero())
                rel_time = rel_type::zero();
            else if (rel_time > std::chrono::seconds(1))
                rel_time = std::chrono::seconds(1);

            std::size_t s = sched_->wait_one(
                static_cast<long>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        rel_time)
                        .count()));

            if (s || stopped())
                return s;

            now = Clock::now();
            if (now >= abs_time)
                return 0;
        }
    }

    /** Process all ready work items without blocking.

        This function executes all work items that are ready to run
        without blocking for more work. The context is stopped
        when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed.
    */
    std::size_t poll()
    {
        return sched_->poll();
    }

    /** Process at most one ready work item without blocking.

        This function executes at most one work item that is ready
        to run without blocking for more work. The context is
        stopped when there is no more outstanding work.

        @note The context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t poll_one()
    {
        return sched_->poll_one();
    }
};

/** An executor for dispatching work to an I/O context.

    The executor provides the interface for posting work items and
    dispatching coroutines to the associated context. It satisfies
    the `capy::Executor` concept.

    Executors are lightweight handles that can be copied and compared
    for equality. Two executors compare equal if they refer to the
    same context.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.
*/
class io_context::executor_type
{
    io_context* ctx_ = nullptr;

public:
    /** Default constructor.

        Constructs an executor not associated with any context.
    */
    executor_type() = default;

    /** Construct an executor from a context.

        @param ctx The context to associate with this executor.
    */
    explicit executor_type(io_context& ctx) noexcept : ctx_(&ctx) {}

    /** Return a reference to the associated execution context.

        @return Reference to the context.
    */
    io_context& context() const noexcept
    {
        return *ctx_;
    }

    /** Check if the current thread is running this executor's context.

        @return `true` if `run()` is being called on this thread.
    */
    bool running_in_this_thread() const noexcept
    {
        return ctx_->sched_->running_in_this_thread();
    }

    /** Informs the executor that work is beginning.

        Must be paired with `on_work_finished()`.
    */
    void on_work_started() const noexcept
    {
        ctx_->sched_->work_started();
    }

    /** Informs the executor that work has completed.

        @par Preconditions
        A preceding call to `on_work_started()` on an equal executor.
    */
    void on_work_finished() const noexcept
    {
        ctx_->sched_->work_finished();
    }

    /** Dispatch a continuation.

        Returns a handle for symmetric transfer. If called from
        within `run()`, returns `c.h`. Otherwise posts the
        enclosing continuation_op as a scheduler_op for later
        execution and returns `std::noop_coroutine()`.

        @param c The continuation to dispatch. Must be the `cont`
                 member of a `detail::continuation_op`.

        @return A handle for symmetric transfer or `std::noop_coroutine()`.
    */
    std::coroutine_handle<> dispatch(capy::continuation& c) const
    {
        if (running_in_this_thread())
            return c.h;
        post(c);
        return std::noop_coroutine();
    }

    /** Post a continuation for deferred execution.

        If the continuation is backed by a continuation_op
        (tagged), posts it directly as a scheduler_op — zero
        heap allocation. Otherwise falls back to the
        heap-allocating post(coroutine_handle<>) path.
    */
    void post(capy::continuation& c) const
    {
        auto* op = detail::continuation_op::try_from_continuation(c);
        if (op)
            ctx_->sched_->post(op);
        else
            ctx_->sched_->post(c.h);
    }

    /** Post a bare coroutine handle for deferred execution.

        Heap-allocates a scheduler_op to wrap the handle. Prefer
        posting through a continuation_op-backed continuation when
        the continuation has suitable lifetime.

        @param h The coroutine handle to post.
    */
    void post(std::coroutine_handle<> h) const
    {
        ctx_->sched_->post(h);
    }

    /** Compare two executors for equality.

        @return `true` if both executors refer to the same context.
    */
    bool operator==(executor_type const& other) const noexcept
    {
        return ctx_ == other.ctx_;
    }

    /** Compare two executors for inequality.

        @return `true` if the executors refer to different contexts.
    */
    bool operator!=(executor_type const& other) const noexcept
    {
        return ctx_ != other.ctx_;
    }
};

inline io_context::executor_type
io_context::get_executor() const noexcept
{
    return executor_type(const_cast<io_context&>(*this));
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_IO_CONTEXT_HPP
