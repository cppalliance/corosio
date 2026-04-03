//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>
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
};

} // namespace boost::corosio::detail

#endif
