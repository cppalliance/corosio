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

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/unique_ptr.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/concept/executor.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace boost {
namespace corosio {

/** An I/O context for running asynchronous operations.

    The io_context provides an execution environment for async operations.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    The nested `executor_type` class provides the interface for dispatching
    coroutines and posting work items. It implements both synchronous
    dispatch (for symmetric transfer) and deferred posting.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, if using a concurrency hint greater than 1.

    @par Example
    @code
    io_context ioc;
    auto ex = ioc.get_executor();
    run_async(ex)(my_coroutine());
    ioc.run();  // Process all queued work
    @endcode
*/
class BOOST_COROSIO_DECL io_context : public capy::execution_context
{
public:
    class executor_type;

    /** Construct an io_context with default concurrency.

        The concurrency hint is set to the number of hardware threads
        available on the system. If more than one thread is available,
        thread-safe synchronization is used.
    */
    io_context();

    /** Construct an io_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads that
            will call `run()`. If greater than 1, thread-safe
            synchronization is used internally.
    */
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

        This causes `run()` to return as soon as possible. Any pending
        work items remain queued.
    */
    void
    stop()
    {
        sched_.stop();
    }

    /** Return whether the io_context has been stopped.

        @return `true` if `stop()` has been called and `restart()`
            has not been called since.
    */
    bool
    stopped() const noexcept
    {
        return sched_.stopped();
    }

    /** Restart the io_context after being stopped.

        This function must be called before `run()` can be called
        again after `stop()` has been called.
    */
    void
    restart()
    {
        sched_.restart();
    }

    /** Process all pending work items.

        This function blocks until all pending work items have been
        executed or `stop()` is called. The io_context is stopped
        when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed.
    */
    std::size_t
    run()
    {
        return sched_.run();
    }

    /** Process at most one pending work item.

        This function blocks until one work item has been executed
        or `stop()` is called. The io_context is stopped when there
        is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t
    run_one()
    {
        return sched_.run_one();
    }

    /** Process work items for the specified duration.

        This function blocks until work items have been executed for
        the specified duration, or `stop()` is called. The io_context
        is stopped when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @param rel_time The duration for which to process work.

        @return The number of handlers executed.
    */
    template<class Rep, class Period>
    std::size_t
    run_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process work items until the specified time.

        This function blocks until the specified time is reached
        or `stop()` is called. The io_context is stopped when there
        is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
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
        The io_context is stopped when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @param rel_time The duration for which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Rep, class Period>
    std::size_t
    run_one_for(std::chrono::duration<Rep, Period> const& rel_time)
    {
        return run_one_until(std::chrono::steady_clock::now() + rel_time);
    }

    /** Process at most one work item until the specified time.

        This function blocks until one work item has been executed,
        the specified time is reached, or `stop()` is called.
        The io_context is stopped when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @param abs_time The time point until which the call may block.

        @return The number of handlers executed (0 or 1).
    */
    template<class Clock, class Duration>
    std::size_t
    run_one_until(std::chrono::time_point<Clock, Duration> const& abs_time)
    {
        typename Clock::time_point now = Clock::now();
        while (now < abs_time)
        {
            auto rel_time = abs_time - now;
            if (rel_time > std::chrono::seconds(1))
                rel_time = std::chrono::seconds(1);

            std::size_t s = sched_.wait_one(
                static_cast<long>(std::chrono::duration_cast<
                    std::chrono::microseconds>(rel_time).count()));

            if (s || stopped())
                return s;

            now = Clock::now();
        }
        return 0;
    }

    /** Process all ready work items without blocking.

        This function executes all work items that are ready to run
        without blocking for more work. The io_context is stopped
        when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

        @return The number of handlers executed.
    */
    std::size_t
    poll()
    {
        return sched_.poll();
    }

    /** Process at most one ready work item without blocking.

        This function executes at most one work item that is ready
        to run without blocking for more work. The io_context is
        stopped when there is no more outstanding work.

        @note The io_context must be restarted with `restart()` before
            calling this function again after it returns.

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

    The executor provides the interface for posting work items and
    dispatching coroutines to the associated io_context. It satisfies
    the `capy::Executor` concept.

    Executors are lightweight handles that can be copied and compared
    for equality. Two executors compare equal if they refer to the
    same io_context.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe.
*/
class io_context::executor_type
{
    io_context* ctx_ = nullptr;

public:
    /** Default constructor.

        Constructs an executor not associated with any io_context.
    */
    executor_type() = default;

    /** Construct an executor from an io_context.

        @param ctx The io_context to associate with this executor.
    */
    explicit
    executor_type(io_context& ctx) noexcept
        : ctx_(&ctx)
    {
    }

    /** Return a reference to the associated execution context.

        @return Reference to the io_context.
    */
    io_context&
    context() const noexcept
    {
        return *ctx_;
    }

    /** Check if the current thread is running this executor's io_context.

        @return `true` if `run()` is being called on this thread.
    */
    bool
    running_in_this_thread() const noexcept
    {
        return ctx_->sched_.running_in_this_thread();
    }

    /** Informs the executor that work is beginning.

        Must be paired with `on_work_finished()`.
    */
    void
    on_work_started() const noexcept
    {
        ctx_->sched_.on_work_started();
    }

    /** Informs the executor that work has completed.

        @par Preconditions
        A preceding call to `on_work_started()` on an equal executor.
    */
    void
    on_work_finished() const noexcept
    {
        ctx_->sched_.on_work_finished();
    }

    /** Dispatch a coroutine handle.

        This is the executor interface for capy coroutines. If called
        from within `run()`, returns the handle for symmetric transfer.
        Otherwise posts the handle and returns `noop_coroutine`.

        @param h The coroutine handle to dispatch.

        @return The handle for symmetric transfer, or `noop_coroutine`
            if the handle was posted.
    */
    capy::coro
    dispatch(capy::coro h) const
    {
        if (running_in_this_thread())
            return h;
        ctx_->sched_.post(h);
        return std::noop_coroutine();
    }

    /** Post a coroutine for deferred execution.

        The coroutine will be resumed during a subsequent call to
        `io_context::run()`.

        @param h The coroutine handle to post.
    */
    void
    post(capy::coro h) const
    {
        ctx_->sched_.post(h);
    }


    /** Compare two executors for equality.

        @return `true` if both executors refer to the same io_context.
    */
    bool
    operator==(executor_type const& other) const noexcept
    {
        return ctx_ == other.ctx_;
    }

    /** Compare two executors for inequality.

        @return `true` if the executors refer to different io_contexts.
    */
    bool
    operator!=(executor_type const& other) const noexcept
    {
        return ctx_ != other.ctx_;
    }
};

//------------------------------------------------------------------------------

inline
auto
io_context::
get_executor() const noexcept ->
    executor_type
{
    return executor_type(const_cast<io_context&>(*this));
}

} // namespace corosio
} // namespace boost

#endif
#endif
