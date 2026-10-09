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

#ifndef BOOST_COROSIO_DETAIL_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <coroutine>
#include <cstddef>
#include <system_error>

namespace boost::corosio::detail {

class scheduler_op;

/** Define the abstract interface for the event loop scheduler.

    Concrete backends (epoll, IOCP, kqueue, select) derive from
    this to implement the reactor/proactor event loop. The
    @ref io_context delegates all scheduling operations here.

    The scheduler is a registry service keyed under this abstract
    type, so services created on first use can locate it without
    naming a concrete backend.

    @see io_context
*/
struct BOOST_COROSIO_DECL scheduler
    : capy::execution_context::service
{
    using key_type = scheduler;

    ~scheduler() override = default;

    /// Post a coroutine handle for deferred execution.
    virtual void post(std::coroutine_handle<>) const = 0;

    /// Post a scheduler operation for deferred execution.
    virtual void post(scheduler_op*) const = 0;

    /// Post a continuation for deferred execution (zero-allocation).
    virtual void post(capy::continuation&) const = 0;

    /// Increment the outstanding work count.
    virtual void work_started() noexcept = 0;

    /// Decrement the outstanding work count.
    virtual void work_finished() noexcept = 0;

    /// Check if the calling thread is running the event loop.
    virtual bool running_in_this_thread() const noexcept = 0;

    /// Signal the event loop to stop.
    virtual void stop() = 0;

    /// Check if the event loop has been stopped.
    virtual bool stopped() const noexcept = 0;

    /// Reset the stopped state so `run()` can be called again.
    virtual void restart() = 0;

    /// Run the event loop, blocking until all work completes.
    virtual std::size_t run() = 0;

    /// Run one handler, blocking until one completes.
    virtual std::size_t run_one() = 0;

    /** Run one handler, blocking up to @p usec microseconds.

        @param usec Maximum wait time in microseconds.

        @return The number of handlers executed (0 or 1).
    */
    virtual std::size_t wait_one(long usec) = 0;

    /// Run all ready handlers without blocking.
    virtual std::size_t poll() = 0;

    /// Run at most one ready handler without blocking.
    virtual std::size_t poll_one() = 0;

    /** Register the read end of the POSIX signal self-pipe.

        Called once (by the first signal_set to register a signal) so the
        backend's event loop watches @p read_fd for readability. When the
        pipe becomes readable the backend drains it and calls
        `posix_signal_service::deliver_signal` for each pending signal, in
        normal thread context. This keeps the C signal handler
        async-signal-safe: it only writes the signal number to the pipe.

        POSIX backends override this; the default is a no-op (Windows/IOCP
        uses synchronous C-runtime signal handling instead).

        @param read_fd The read end of the global signal self-pipe.

        @return The error code, empty on success.
    */
    [[nodiscard]] virtual std::error_code
    register_signal_reader([[maybe_unused]] int read_fd)
    {
        return {}; // LCOV_EXCL_LINE POSIX overrides; the only caller never runs on IOCP
    }

    /// Decomposed threading configuration applied via @ref configure_threading.
    struct threading_config
    {
        /// Scheduler mutex/condvar enabled. Off only in the `unsafe` tier.
        bool scheduler_locking = true;
        /// Per-descriptor (reactor) or ring (uring) I/O lock enabled.
        /// Off in the `unsafe_io` and `unsafe` tiers.
        bool reactor_io_locking = true;
        /// A single run thread is guaranteed (a lockless tier): elide
        /// inter-run-thread wakeups.
        bool one_thread = false;
    };

    /// True in the fully-lockless (`unsafe`) tier. The resolver and POSIX
    /// file services gate their `operation_not_supported` result on this.
    virtual bool scheduler_locking_disabled() const noexcept = 0;

    /// Apply @ref threading_config.
    virtual void configure_threading(threading_config) noexcept = 0;
};

/** Return the scheduler registered with the context.

    @throws std::logic_error If the context has no backend installed.
*/
/** The innermost scheduler running on this thread, or null.

    Each backend's run guard sets it and restores the previous value,
    so a completion can ask whether its awaiter's context is running
    here with one compare. Nested runs of other schedulers are still
    answered by the scheduler's own frame stack.
*/
inline thread_local_ptr<scheduler const> running_scheduler;

/** RAII guard marking a scheduler as innermost on this thread.

    Restores the previous innermost scheduler on destruction.
*/
class running_scheduler_guard
{
    scheduler const* prev_;

public:
    /// Construct the guard, making @p s the innermost scheduler.
    explicit running_scheduler_guard(scheduler const* s) noexcept
        : prev_(running_scheduler.get())
    {
        running_scheduler.set(s);
    }

    /// Destroy the guard, restoring the previous innermost scheduler.
    ~running_scheduler_guard()
    {
        running_scheduler.set(prev_);
    }

    running_scheduler_guard(running_scheduler_guard const&)            = delete;
    running_scheduler_guard& operator=(running_scheduler_guard const&) = delete;
};

inline scheduler&
get_scheduler(capy::execution_context& ctx)
{
    auto* sched = ctx.find_service<scheduler>();
    if (!sched)
        throw_logic_error("no scheduler installed");
    return *sched;
}

} // namespace boost::corosio::detail

#endif
