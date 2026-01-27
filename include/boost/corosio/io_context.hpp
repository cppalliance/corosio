//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_CONTEXT_HPP
#define BOOST_COROSIO_IO_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/basic_io_context.hpp>

// Include the platform-specific context headers
#if defined(_WIN32)
#include <boost/corosio/iocp_context.hpp>
#elif defined(__linux__)
#include <boost/corosio/epoll_context.hpp>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__APPLE__)
// kqueue_context would be included here when implemented
// #include <boost/corosio/kqueue_context.hpp>
#include <boost/corosio/epoll_context.hpp>  // Placeholder - kqueue not yet implemented
#else
// select_context would be included here when implemented
// #include <boost/corosio/select_context.hpp>
#include <boost/corosio/epoll_context.hpp>  // Placeholder - select not yet implemented
#endif

namespace boost::corosio {

/** An I/O context for running asynchronous operations.

    The io_context provides an execution environment for async operations.
    It maintains a queue of pending work items and processes them when
    `run()` is called.

    This is a type alias for the platform's default I/O backend:
    - Windows: `iocp_context` (I/O Completion Ports)
    - Linux: `epoll_context` (epoll)
    - BSD/macOS: `kqueue_context` (kqueue) [future]
    - Other POSIX: `select_context` (select) [future]

    For explicit backend selection, use the concrete context types
    directly (e.g., `epoll_context`, `iocp_context`).

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

    @par Explicit Backend Selection
    @code
    // Use epoll explicitly (Linux)
    epoll_context ctx;

    // Generic code using IoContext concept
    template<IoContext Ctx>
    void run_server(Ctx& ctx) {
        ctx.run();
    }
    @endcode
*/
#if defined(_WIN32)
using io_context = iocp_context;
#elif defined(__linux__)
using io_context = epoll_context;
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__APPLE__)
// kqueue_context when implemented
using io_context = epoll_context;  // Placeholder
#else
// select_context when implemented
using io_context = epoll_context;  // Placeholder
#endif

} // namespace boost::corosio

#endif // BOOST_COROSIO_IO_CONTEXT_HPP
