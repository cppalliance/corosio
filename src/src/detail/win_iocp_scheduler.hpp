//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_IOCP_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_WIN_IOCP_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

#include <boost/corosio/io_context.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/capy/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>

namespace boost {
namespace corosio {
namespace detail {

/** Windows IOCP-based scheduler service.

    This scheduler uses Windows I/O Completion Ports (IOCP) to manage
    asynchronous work items. Work items are posted to the completion
    port and dequeued during run() calls.

    IOCP provides efficient, scalable I/O completion notification and
    is the foundation for high-performance Windows I/O. This scheduler
    leverages IOCP's thread-safe completion queue for work dispatch.

    @par Thread Safety
    This implementation is inherently thread-safe. Multiple threads
    may call post() concurrently, and multiple threads may call
    run() to dequeue and execute work items.

    @par Usage
    @code
    io_context ctx;
    auto& sched = ctx.use_service<detail::win_iocp_scheduler>();
    // ... post work via scheduler interface
    ctx.run();  // Processes work via IOCP
    @endcode

    @note Only available on Windows platforms.

    @see detail::scheduler
*/
class win_iocp_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    /** Constructs a Windows IOCP scheduler.

        Creates an I/O Completion Port for managing work items.

        @param ctx Reference to the owning execution_context.

        @throws std::system_error if IOCP creation fails.
    */
     win_iocp_scheduler(
        capy::execution_context& ctx,
        unsigned concurrency_hint = 0);

    /** Destroys the scheduler and releases IOCP resources.

        Any pending work items are destroyed without execution.
    */
    ~win_iocp_scheduler();

    win_iocp_scheduler(win_iocp_scheduler const&) = delete;
    win_iocp_scheduler& operator=(win_iocp_scheduler const&) = delete;

    /** Shuts down the scheduler.

        Signals the IOCP to wake blocked threads and destroys any
        remaining work items without executing them.
    */
    void shutdown() override;

    /** Posts a coroutine for later execution.

        @param h The coroutine handle to post.
    */
    void post(capy::coro h) const override;

    /** Posts a work item for later execution.

        Posts the work item to the IOCP. The item will be dequeued
        and executed during a subsequent call to run().

        @param w Pointer to the work item. Ownership is transferred
                 to the scheduler.

        @par Thread Safety
        This function is thread-safe.
    */
    void post(capy::executor_work* w) const override;

    /** Check if the current thread is running this scheduler.

        @return true if run() is being called on this thread.
    */
    bool running_in_this_thread() const noexcept override;

    /** Signal the scheduler to stop processing.

        This causes run() to return as soon as possible.
    */
    void stop() override;

    /** Return whether the scheduler has been stopped.

        @return true if stop() has been called and restart()
            has not been called since.
    */
    bool stopped() const noexcept override;

    /** Restart the scheduler after being stopped.

        This function must be called before run() can be called
        again after stop() has been called.
    */
    void restart() override;

    /** Processes pending work items.

        Dequeues all available completions from the IOCP and executes
        them. Returns when stopped or no more work is available.

        @param ec Set to indicate any error.

        @return The number of handlers executed.

        @par Thread Safety
        This function is thread-safe. Multiple threads may call
        run() concurrently.
    */
    std::size_t run(system::error_code& ec) override;

    /** Processes at most one pending work item.

        Blocks until one work item is executed or stop() is called.

        @param ec Set to indicate any error.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t run_one(system::error_code& ec) override;

    /** Processes at most one pending work item with timeout.

        Blocks until one work item is executed, the timeout expires,
        or stop() is called.

        @param usec Timeout in microseconds.
        @param ec Set to indicate any error.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t run_one(long usec, system::error_code& ec) override;

    /** Wait for at most one completion without executing.

        Blocks until a completion is available, the timeout expires,
        or stop() is called. The completion is not executed.

        @param usec Timeout in microseconds.
        @param ec Set to indicate any error.

        @return The number of completions available (0 or 1).
    */
    std::size_t wait_one(long usec, system::error_code& ec) override;

    /** Processes work items for the specified duration.

        @param rel_time The duration for which to process work.

        @return The number of handlers executed.
    */
    std::size_t run_for(std::chrono::steady_clock::duration rel_time) override;

    /** Processes work items until the specified time.

        @param abs_time The time point until which to process work.

        @return The number of handlers executed.
    */
    std::size_t run_until(std::chrono::steady_clock::time_point abs_time) override;

    /** Processes all ready work items without blocking.

        @param ec Set to indicate any error.

        @return The number of handlers executed.
    */
    std::size_t poll(system::error_code& ec) override;

    /** Processes at most one ready work item without blocking.

        @param ec Set to indicate any error.

        @return The number of handlers executed (0 or 1).
    */
    std::size_t poll_one(system::error_code& ec) override;

    /** Returns the native IOCP handle.

        @return The Windows HANDLE to the I/O Completion Port.
    */
    void* native_handle() const noexcept { return iocp_; }

    /** Notify scheduler that an I/O operation has started.

        This increments the pending work count. Must be called
        before initiating async I/O that will complete on the IOCP.
    */
    void work_started() const noexcept
    {
        pending_.fetch_add(1, std::memory_order_relaxed);
    }

    /** Notify scheduler that an I/O operation was abandoned.

        This decrements the pending work count. Called when an
        async I/O fails synchronously without posting to IOCP.
    */
    void work_finished() const noexcept
    {
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::size_t do_run(unsigned long timeout, std::size_t max_handlers,
        system::error_code& ec);
    std::size_t do_wait(unsigned long timeout, system::error_code& ec);

    void* iocp_;
    mutable std::atomic<std::size_t> pending_{0};
    std::atomic<bool> stopped_{false};
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
