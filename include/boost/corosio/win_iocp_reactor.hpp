//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WIN_IOCP_REACTOR_HPP
#define BOOST_COROSIO_WIN_IOCP_REACTOR_HPP

#include <boost/corosio/platform_reactor.hpp>

#ifdef _WIN32

namespace boost {
namespace corosio {

/** Windows IOCP-based platform reactor service.

    This reactor uses Windows I/O Completion Ports (IOCP) to manage
    asynchronous work items. Work items are posted to the completion
    port and dequeued during process() calls.

    IOCP provides efficient, scalable I/O completion notification and
    is the foundation for high-performance Windows I/O. This reactor
    leverages IOCP's thread-safe completion queue for work dispatch.

    @par Thread Safety
    This implementation is inherently thread-safe. Multiple threads
    may call submit() concurrently, and multiple threads may call
    process() to dequeue and execute work items.

    @par Usage
    @code
    io_context ctx;
    ctx.make_service<win_iocp_reactor>();
    // ... submit work via platform_reactor interface
    ctx.run();  // Processes work via IOCP
    @endcode

    @note Only available on Windows platforms.

    @see platform_reactor
*/
class win_iocp_reactor : public platform_reactor
{
public:
    using key_type = platform_reactor;

    /** Constructs a Windows IOCP reactor.

        Creates an I/O Completion Port for managing work items.

        @param sp Reference to the owning service_provider.

        @throws std::system_error if IOCP creation fails.
    */
    explicit win_iocp_reactor(capy::service_provider& sp);

    /** Destroys the reactor and releases IOCP resources.

        Any pending work items are destroyed without execution.
    */
    ~win_iocp_reactor();

    win_iocp_reactor(win_iocp_reactor const&) = delete;
    win_iocp_reactor& operator=(win_iocp_reactor const&) = delete;

    /** Shuts down the reactor.

        Signals the IOCP to wake blocked threads and destroys any
        remaining work items without executing them.
    */
    void shutdown() override;

    /** Submits a work item for later execution.

        Posts the work item to the IOCP. The item will be dequeued
        and executed during a subsequent call to process().

        @param w Pointer to the work item. Ownership is transferred
                 to the reactor.

        @par Thread Safety
        This function is thread-safe.
    */
    void submit(capy::executor_work* w) override;

    /** Processes pending work items.

        Dequeues all available completions from the IOCP and executes
        them. Returns when no more completions are immediately available.

        @par Thread Safety
        This function is thread-safe. Multiple threads may call
        process() concurrently.
    */
    void process() override;

    /** Returns the native IOCP handle.

        @return The Windows HANDLE to the I/O Completion Port.
    */
    void* native_handle() const noexcept { return iocp_; }

private:
    void* iocp_;
};

} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
