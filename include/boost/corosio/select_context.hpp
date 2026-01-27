//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SELECT_CONTEXT_HPP
#define BOOST_COROSIO_SELECT_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>

// select_context is available on all POSIX platforms
#if !defined(_WIN32)

#include <boost/corosio/basic_io_context.hpp>

namespace boost::corosio {

/** I/O context using POSIX select() for event multiplexing.

    This context provides an execution environment for async operations
    using the POSIX select() API for I/O event notification. It is
    available on all POSIX platforms and provides a portable fallback
    when more efficient platform-specific APIs (epoll, kqueue) are
    not available or when explicit portability is desired.

    On Linux, both `epoll_context` and `select_context` are available,
    allowing users to choose at runtime:

    @code
    epoll_context ctx1;   // Use epoll (best performance)
    select_context ctx2;  // Use select (portable, useful for testing)
    @endcode

    @par Known Limitations
    - FD_SETSIZE (~1024) limits maximum concurrent connections
    - O(n) scanning: rebuilds fd_sets each iteration
    - Level-triggered only (no edge-triggered mode)

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Safe, if using a concurrency hint greater than 1.

    @par Example
    @code
    select_context ctx;
    auto ex = ctx.get_executor();
    run_async(ex)(my_coroutine());
    ctx.run();  // Process all queued work
    @endcode
*/
class BOOST_COROSIO_DECL select_context : public basic_io_context
{
public:
    /** Construct a select_context with default concurrency.

        The concurrency hint is set to the number of hardware threads
        available on the system. If more than one thread is available,
        thread-safe synchronization is used.
    */
    select_context();

    /** Construct a select_context with a concurrency hint.

        @param concurrency_hint A hint for the number of threads that
            will call `run()`. If greater than 1, thread-safe
            synchronization is used internally.
    */
    explicit
    select_context(unsigned concurrency_hint);

    /** Destructor. */
    ~select_context();

    // Non-copyable
    select_context(select_context const&) = delete;
    select_context& operator=(select_context const&) = delete;
};

} // namespace boost::corosio

#endif // !defined(_WIN32)

#endif // BOOST_COROSIO_SELECT_CONTEXT_HPP
