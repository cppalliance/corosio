//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WIN_IOCP_SCHEDULER_HPP
#define BOOST_COROSIO_WIN_IOCP_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

#include <boost/corosio/io_context.hpp>
#include <boost/capy/execution_context.hpp>

#include <thread>

namespace boost {
namespace corosio {

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
    auto& sched = ctx.use_service<win_iocp_scheduler>();
    // ... post work via scheduler interface
    ctx.run();  // Processes work via IOCP
    @endcode

    @note Only available on Windows platforms.

    @see detail::scheduler
*/
class win_iocp_scheduler
    : public detail::scheduler
    , public capy::execution_context::service
{
public:
    using key_type = detail::scheduler;

    /** Constructs a Windows IOCP scheduler.

        Creates an I/O Completion Port for managing work items.

        @param ctx Reference to the owning execution_context.

        @throws std::system_error if IOCP creation fails.
    */
    explicit win_iocp_scheduler(capy::execution_context& ctx);

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
    void stop() const override;

    /** Processes pending work items.

        Dequeues all available completions from the IOCP and executes
        them. Returns when no more completions are immediately available.

        @par Thread Safety
        This function is thread-safe. Multiple threads may call
        run() concurrently.
    */
    void run() const override;

    /** Returns the native IOCP handle.

        @return The Windows HANDLE to the I/O Completion Port.
    */
    void* native_handle() const noexcept { return iocp_; }

private:
    void* iocp_;
    std::thread::id thread_id_;
};

} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
