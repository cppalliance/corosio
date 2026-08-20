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

#include <system_error>
#include <boost/capy/continuation.hpp>
#include <coroutine>

#include <cstddef>

namespace boost::corosio::detail {

class scheduler_op;

/** Define the abstract interface for the event loop scheduler.

    Concrete backends (epoll, IOCP, kqueue, select) derive from
    this to implement the reactor/proactor event loop. The
    @ref io_context delegates all scheduling operations here.

    @see io_context
*/
struct BOOST_COROSIO_DECL scheduler
{
    virtual ~scheduler() = default;

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
    register_signal_reader(int read_fd)
    {
        (void)read_fd;
        return {};
    }

    /// Decomposed threading configuration applied via @ref configure_threading.
    struct threading_config
    {
        /// Scheduler mutex/condvar enabled. Off only in the `unsafe` tier.
        bool scheduler_locking  = true;
        /// Per-descriptor (reactor) or ring (io_uring) I/O lock enabled.
        /// Off in the `unsafe_io` and `unsafe` tiers.
        bool reactor_io_locking = true;
        /// A single run thread is guaranteed (a lockless tier): elide
        /// inter-run-thread wakeups.
        bool one_thread         = false;
    };

    /// True in the fully-lockless (`unsafe`) tier. The resolver and POSIX
    /// file services gate their `operation_not_supported` result on this.
    virtual bool scheduler_locking_disabled() const noexcept { return false; }

    /// Apply @ref threading_config. Default no-op.
    virtual void configure_threading(threading_config) noexcept {}
};

} // namespace boost::corosio::detail

#endif
