//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_KQUEUE_CONTEXT_HPP
#define BOOST_COROSIO_KQUEUE_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/basic_io_context.hpp>

namespace boost::corosio {

/** I/O context using BSD kqueue for event multiplexing.

    This context provides an execution environment for async operations
    using the BSD kqueue API for efficient I/O event notification.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe. Internal synchronization is always present
    regardless of the concurrency hint.

    @par Example
    @code
    kqueue_context ctx;
    auto ex = ctx.get_executor();
    run_async(ex)(my_coroutine());
    ctx.run();  // Process all queued work
    @endcode

    @see basic_io_context, basic_io_context::get_executor,
         basic_io_context::run, capy::execution_context
*/
class BOOST_COROSIO_DECL kqueue_context : public basic_io_context
{
public:
    /** Construct a kqueue_context with default concurrency.

        The concurrency hint is set to the number of hardware threads
        available on the system. If more than one thread is available,
        thread-safe synchronization is used.

        @throws std::system_error if creating the kqueue file descriptor
            or registering the EVFILT_USER interrupt event fails.
    */
    kqueue_context();

    /** Construct a kqueue_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads that
            will call `run()`. If greater than 1, thread-safe
            synchronization is used internally.

        @throws std::system_error if creating the kqueue file descriptor
            or registering the EVFILT_USER interrupt event fails.
    */
    explicit
    kqueue_context(unsigned concurrency_hint);

    /** Destructor.

        Calls `shutdown()` and `destroy()` to release all resources.
        Does not throw.
    */
    ~kqueue_context();

    // Non-copyable, non-movable
    kqueue_context(kqueue_context const&) = delete;
    kqueue_context& operator=(kqueue_context const&) = delete;
    kqueue_context(kqueue_context&&) = delete;
    kqueue_context& operator=(kqueue_context&&) = delete;
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_KQUEUE_CONTEXT_HPP
