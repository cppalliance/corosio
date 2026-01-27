//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_EPOLL_CONTEXT_HPP
#define BOOST_COROSIO_EPOLL_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>

// epoll_context is only available on Linux
#if defined(__linux__)

#include <boost/corosio/basic_io_context.hpp>

namespace boost::corosio {

/** I/O context using Linux epoll for event multiplexing.

    This context provides an execution environment for async operations
    using the Linux epoll API for efficient I/O event notification.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, if using a concurrency hint greater than 1.

    @par Example
    @code
    epoll_context ctx;
    auto ex = ctx.get_executor();
    run_async(ex)(my_coroutine());
    ctx.run();  // Process all queued work
    @endcode
*/
class BOOST_COROSIO_DECL epoll_context : public basic_io_context
{
public:
    /** Construct an epoll_context with default concurrency.

        The concurrency hint is set to the number of hardware threads
        available on the system. If more than one thread is available,
        thread-safe synchronization is used.
    */
    epoll_context();

    /** Construct an epoll_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads that
            will call `run()`. If greater than 1, thread-safe
            synchronization is used internally.
    */
    explicit
    epoll_context(unsigned concurrency_hint);

    /** Destructor. */
    ~epoll_context();

    // Non-copyable
    epoll_context(epoll_context const&) = delete;
    epoll_context& operator=(epoll_context const&) = delete;
};

} // namespace boost::corosio

#endif // defined(__linux__)

#endif // BOOST_COROSIO_EPOLL_CONTEXT_HPP
