//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_CONTEXT_HPP
#define BOOST_COROSIO_IO_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/unique_ptr.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/executor.hpp>
#include <boost/capy/execution_context.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <utility>

namespace boost {
namespace corosio {

namespace detail {

struct scheduler
{
    virtual ~scheduler() = default;
    virtual void post(capy::coro) const = 0;
    virtual void post(capy::executor_work*) const = 0;
    virtual bool running_in_this_thread() const noexcept = 0;
    virtual void stop() = 0;
    virtual bool stopped() const noexcept = 0;
    virtual void restart() = 0;
    virtual std::size_t run() = 0;
    virtual std::size_t run_one() = 0;
    virtual std::size_t run_for(std::chrono::steady_clock::duration) = 0;
    virtual std::size_t run_until(std::chrono::steady_clock::time_point) = 0;
    virtual std::size_t poll() = 0;
    virtual std::size_t poll_one() = 0;
};

} // namespace detail

/** A simple I/O context for running asynchronous operations.

    The io_context provides an execution environment for async operations.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    The nested `executor` type provides the interface for dispatching
    coroutines and posting work items. It implements both synchronous
    dispatch (for symmetric transfer) and deferred posting.

    @par Example
    @code
    io_context ioc;
    auto ex = ioc.get_executor();
    async_run(ex, my_coroutine());
    ioc.run();  // Process all queued work
    @endcode

    @note This is a simplified implementation for benchmarking purposes.
    Production implementations would integrate with OS-level async I/O.

    @see executor_work
    @see executor_work_queue
    @see executor_base
    @see execution_context
*/
class io_context : public capy::execution_context
{
public:
    class executor;

    /// The executor type for this io_context.
    using executor_type = executor;

    /** Construct an io_context.

        The concurrency hint is set to the number of hardware
        threads available on the system. If more than one thread
        is available, thread-safe synchronization is used.
    */
    BOOST_COROSIO_DECL
    io_context();

    /** Construct an io_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads
        that will call run(). If greater than 1, thread-safe
        synchronization is used internally.
    */
    BOOST_COROSIO_DECL
    explicit
    io_context(unsigned concurrency_hint);

    /** Return an executor for this io_context.

        The returned executor can be used to dispatch coroutines
        and post work items to this io_context.

        @return An executor associated with this io_context.
    */
    executor_type
    get_executor() const noexcept;

    /** Signal the io_context to stop processing.

        This causes run() to return as soon as possible.
        Any pending work items remain queued.
    */
    void
    stop()
    {
        sched_.stop();
    }

    /** Return whether the io_context has been stopped.

        @return true if stop() has been called and restart()
            has not been called since.
    */
    bool
    stopped() const noexcept
    {
        return sched_.stopped();
    }

    /** Restart the io_context after being stopped.

        This function must be called before run() can be called
        again after stop() has been called.
    */
    void
    restart()
    {
        sched_.restart();
    }

    /** Process all pending work items.

        This function blocks until all pending work items have
        been executed or stop() is called.

        @return The number of handlers executed.
    */
    std::size_t
    run()
    {
        return sched_.run();
    }

    /** Process at most one pending work item.

        This function blocks until one work item has been
        executed or stop() is called.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t
    run_one()
    {
        return sched_.run_one();
    }

    /** Process work items for the specified duration.

        This function blocks until work items have been executed
        for the specified duration, or stop() is called.

        @param rel_time The duration for which to process work.

        @return The number of handlers executed.
    */
    template<class Rep, class Period>
    std::size_t
    run_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return sched_.run_for(
            std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(rel_time));
    }

    /** Process work items until the specified time.

        This function blocks until work items have been executed
        until the specified time, or stop() is called.

        @param abs_time The time point until which to process work.

        @return The number of handlers executed.
    */
    template<class Clock, class Duration>
    std::size_t
    run_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        return run_for(abs_time - Clock::now());
    }

    /** Process all ready work items without blocking.

        This function executes all work items that are ready
        to run without blocking for more work.

        @return The number of handlers executed.
    */
    std::size_t
    poll()
    {
        return sched_.poll();
    }

    /** Process at most one ready work item without blocking.

        This function executes at most one work item that is
        ready to run without blocking for more work.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t
    poll_one()
    {
        return sched_.poll_one();
    }

private:
    detail::scheduler& sched_;
};

//------------------------------------------------------------------------------

/** An executor for dispatching work to an io_context.

    The executor provides the interface for posting work items
    and dispatching coroutines to the associated io_context.
    It satisfies the dispatcher concept required by capy coroutines.

    Executors are lightweight handles that can be copied and
    compared for equality. Two executors compare equal if they
    refer to the same io_context.
*/
class io_context::executor
{
    detail::scheduler* sched_ = nullptr;

public:
    /** Default constructor.

        Constructs an executor not associated with any io_context.
    */
    executor() = default;

    /** Construct an executor from a scheduler.

        @param sched The scheduler to associate with this executor.
    */
    explicit
    executor(detail::scheduler& sched) noexcept
        : sched_(&sched)
    {
    }

    /** Check if the current thread is running this executor's io_context.

        @return true if run() is being called on this thread.
    */
    bool
    running_in_this_thread() const noexcept
    {
        return sched_->running_in_this_thread();
    }

    /** Dispatch a coroutine handle.

        This is the dispatcher interface for capy coroutines.
        If called from within run(), returns the handle for
        symmetric transfer. Otherwise posts the handle and
        returns noop_coroutine.

        @param h The coroutine handle to dispatch.

        @return The handle for symmetric transfer, or noop_coroutine
        if the handle was posted.
    */
    capy::coro
    operator()(capy::coro h) const
    {
        return dispatch(h);
    }

    /** Dispatch a coroutine handle.

        If called from within run(), returns the handle for
        symmetric transfer. Otherwise posts the handle and
        returns noop_coroutine.

        @param h The coroutine handle to dispatch.

        @return The handle for symmetric transfer, or noop_coroutine
        if the handle was posted.
    */
    capy::coro
    dispatch(capy::coro h) const
    {
        if (running_in_this_thread())
            return h;
        sched_->post(h);
        return std::noop_coroutine();
    }

    /** Post a work item for deferred execution.

        The work item will be executed during a subsequent
        call to io_context::run().

        @param w The work item to post. Ownership is transferred.
    */
    void
    post(capy::executor_work* w) const
    {
        sched_->post(w);
    }

    /** Compare two executors for equality.

        @return true if both executors refer to the same io_context.
    */
    bool
    operator==(executor const& other) const noexcept
    {
        return sched_ == other.sched_;
    }

    /** Compare two executors for inequality.

        @return true if the executors refer to different io_contexts.
    */
    bool
    operator!=(executor const& other) const noexcept
    {
        return sched_ != other.sched_;
    }
};

//------------------------------------------------------------------------------

inline
auto
io_context::
get_executor() const noexcept ->
    executor_type
{
    return executor_type(sched_);
}

} // namespace corosio
} // namespace boost

#endif
