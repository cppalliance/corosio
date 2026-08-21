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

/** Locking-safety tier for an @ref io_context.

    Selects which internal locks the scheduler and reactor elide, trading
    thread-safety guarantees for reduced synchronization overhead. This is
    the analog of Boost.Asio's `SAFE` / `UNSAFE_IO` / `UNSAFE` concurrency
    hint constants. The tier is chosen explicitly, not derived from the
    `concurrency_hint`. (The reverse does apply: a lockless tier reduces the
    effective hint used for performance tuning to 1.)

    @see io_context_options::locking
*/
enum class locking_mode
{
    /** Full thread safety (default). All locks enabled; equivalent to
        Boost.Asio's `SAFE`/`DEFAULT`. Any thread may use the context. */
    safe,

    /** Disable only the per-descriptor I/O locks; keep scheduler locking.
        Equivalent to Boost.Asio's `UNSAFE_IO`. The context must be run
        and driven by a single thread, but resolver and POSIX file
        services remain available (they rely on scheduler locking, which
        stays on). */
    unsafe_io,

    /** Disable all locking (fully lockless). Equivalent to Boost.Asio's
        `UNSAFE`.

        @par Restrictions
        - Only one thread may call `run()` (or any run variant).
        - Posting work from another thread is undefined behavior.
        - DNS resolution returns `operation_not_supported`.
        - POSIX file I/O returns `operation_not_supported`.
        - Signal sets should not be shared across contexts. */
    unsafe
};

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

    /** Thread pool size for blocking I/O (file I/O, DNS resolution).

        Sets the number of worker threads in the shared thread pool
        used by POSIX file services and DNS resolution. Must be at
        least 1. Applies to POSIX backends only; ignored on IOCP
        where file I/O uses native overlapped I/O.
    */
    unsigned thread_pool_size = 1;

    /** Thread-safety tier. See @ref locking_mode for the tiers and their
        restrictions.
    */
    locking_mode locking = locking_mode::safe;

    /** Enable IORING_SETUP_SQPOLL on the io_uring backend.

        With SQPOLL, the kernel forks a thread that busy-polls the
        submission ring; submission becomes a userspace-only memory
        store, eliminating the io_uring_enter syscall on the submit
        path. Most useful for sustained traffic. Idle thread parks
        after `sq_thread_idle_ms` of no activity.

        Independent of `locking`. Default: off.

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

/** Return the hint used for performance tuning: the lockless tiers are
    single-threaded, so their effective hint is 1 whatever the caller passed.
*/
inline unsigned
effective_concurrency_hint(
    io_context_options const& opts, unsigned hint) noexcept
{
    return opts.locking == locking_mode::safe ? hint : 1u;
}
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

    @par Preconditions
    The context must outlive every operation posted or dispatched
    through its executor, and no thread may be executing a run
    variant when the context is destroyed. Posting to the context
    concurrently with, or after, its destruction is undefined
    behavior. The safe teardown pattern is to stop submitting new
    work, let every `run()` call return (each returns once no
    outstanding work remains), and join the threads that ran the
    loop before destroying the context. Work launched with
    `capy::run` / `capy::run_async` is work-tracked, so a normal
    `run()` completion already waits for it.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, unless the context was constructed with a
    lockless @ref io_context_options::locking tier (`unsafe_io` or
    `unsafe`), in which case a single thread must drive it.

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

    /** Apply only the decomposed threading configuration (locking tiers).
        Used by the plain constructors, which — unlike the options
        constructors — deliberately leave the reactor budget at its defaults
        rather than engaging the multi-thread post-everything heuristic. */
    void apply_threading_(io_context_options const& opts);

protected:
    detail::scheduler* sched_;

public:
    /** The executor type for this context. */
    class executor_type;

    /** Construct with default concurrency and platform backend.

        Uses `std::thread::hardware_concurrency()` (floored to 1, in
        case it reports 0) as the concurrency hint, and the default
        @ref locking_mode::safe tier. Select a lockless tier via
        @ref io_context_options::locking.
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
        [[maybe_unused]] Backend backend,
        unsigned concurrency_hint = std::thread::hardware_concurrency())
        : capy::execution_context(this)
        , sched_(nullptr)
    {
        sched_ = &Backend::construct(*this, concurrency_hint);
        // Apply threading config only (locking tier). Unlike the options
        // ctor, the plain path leaves the reactor budget at its defaults.
        apply_threading_(io_context_options{});
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
        [[maybe_unused]] Backend backend,
        io_context_options const& opts,
        unsigned concurrency_hint = std::thread::hardware_concurrency())
        : capy::execution_context(this)
        , sched_(nullptr)
    {
        apply_options_pre_(opts);
        // Effective hint (1 for lockless tiers); see effective_concurrency_hint.
        unsigned const eff =
            detail::effective_concurrency_hint(opts, concurrency_hint);
        sched_ = &Backend::construct(*this, eff);
        apply_options_post_(opts, eff);
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
        within `run()`, returns `c.h`. Otherwise posts `c` for
        later execution and returns `std::noop_coroutine()`.

        @param c The continuation to dispatch.

        @return A handle for symmetric transfer or `std::noop_coroutine()`.

        @par Preconditions
        The associated context must outlive this call. Dispatching
        concurrently with, or after, the context's destruction is
        undefined behavior.
    */
    std::coroutine_handle<> dispatch(capy::continuation& c) const
    {
        if (running_in_this_thread())
            return c.h;
        post(c);
        return std::noop_coroutine();
    }

    /** Post a continuation for deferred execution.

        Enqueues `c` directly on the scheduler's ready queue.
        No heap allocation occurs.

        @par Preconditions
        The associated context must outlive this call. Posting
        concurrently with, or after, the context's destruction is
        undefined behavior.
    */
    void post(capy::continuation& c) const
    {
        ctx_->sched_->post(c);
    }

    /** Post a bare coroutine handle for deferred execution.

        Heap-allocates a scheduler_op to wrap the handle. A caller
        that already owns a `scheduler_op` can post it directly via
        the `post(scheduler_op*)` overload to avoid the allocation.

        @param h The coroutine handle to post.

        @par Preconditions
        The associated context must outlive this call. Posting
        concurrently with, or after, the context's destruction is
        undefined behavior.
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
