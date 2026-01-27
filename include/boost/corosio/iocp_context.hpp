//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IOCP_CONTEXT_HPP
#define BOOST_COROSIO_IOCP_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>

// iocp_context is only available on Windows
#if defined(_WIN32)

#include <boost/corosio/basic_io_context.hpp>

namespace boost {
namespace corosio {

/** I/O context using Windows I/O Completion Ports for event multiplexing.

    This context provides an execution environment for async operations
    using the Windows I/O Completion Ports (IOCP) API for efficient
    I/O event notification. It maintains a queue of pending work items
    and processes them when `run()` is called.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, if using a concurrency hint greater than 1.

    @par Example
    @code
    iocp_context ctx;
    auto ex = ctx.get_executor();
    run_async(ex)(my_coroutine());
    ctx.run();  // Process all queued work
    @endcode
*/
class BOOST_COROSIO_DECL iocp_context : public basic_io_context
{
public:
    /** Construct an iocp_context with default concurrency.

        The concurrency hint is set to the number of hardware threads
        available on the system. If more than one thread is available,
        thread-safe synchronization is used.
    */
    iocp_context();

    /** Construct an iocp_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads that
            will call `run()`. If greater than 1, thread-safe
            synchronization is used internally.
    */
    explicit
    iocp_context(unsigned concurrency_hint);

    /** Destructor. */
    ~iocp_context();

    // Non-copyable
    iocp_context(iocp_context const&) = delete;
    iocp_context& operator=(iocp_context const&) = delete;
};

} // namespace corosio
} // namespace boost

#endif // defined(_WIN32)

#endif // BOOST_COROSIO_IOCP_CONTEXT_HPP
