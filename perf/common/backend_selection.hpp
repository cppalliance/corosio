//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_PERF_BACKEND_SELECTION_HPP
#define BOOST_COROSIO_PERF_BACKEND_SELECTION_HPP

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/backend.hpp>

#include <cstring>
#include <iostream>
#include <memory>

namespace perf {

/// Factory function pointer that creates a fresh io_context.
using context_factory = std::unique_ptr<boost::corosio::io_context> (*)();

/** Return the default backend name for the current platform. */
inline const char*
default_backend_name()
{
#if BOOST_COROSIO_HAS_IOCP
    return "iocp";
#elif BOOST_COROSIO_HAS_EPOLL
    return "epoll";
#elif BOOST_COROSIO_HAS_KQUEUE
    return "kqueue";
#elif BOOST_COROSIO_HAS_SELECT
    return "select";
#else
    return "unknown";
#endif
}

/** Print available backends for the current platform. */
inline void
print_available_backends()
{
    std::cout << "Available backends on this platform:\n";
#if BOOST_COROSIO_HAS_IOCP
    std::cout << "  iocp     - Windows I/O Completion Ports (default)\n";
#endif
#if BOOST_COROSIO_HAS_IO_URING
    std::cout << "  io_uring - Linux io_uring\n";
#endif
#if BOOST_COROSIO_HAS_EPOLL
    std::cout << "  epoll    - Linux epoll (default)\n";
#endif
#if BOOST_COROSIO_HAS_KQUEUE
    std::cout << "  kqueue   - BSD/macOS kqueue (default)\n";
#endif
#if BOOST_COROSIO_HAS_SELECT
    std::cout << "  select   - POSIX select (portable)\n";
#endif
    std::cout << "\nDefault backend: " << default_backend_name() << "\n";
}

/** Dispatch to a function based on backend name.

    Resolves the backend name to a context_factory and passes it
    to the callback along with the backend tag and canonical name.

    @param backend The backend name (epoll, select, iocp, etc.)
    @param func A callable with signature
        `void(context_factory, Backend, char const*)`
    @return 0 on success, 1 if backend is not available
*/
template<typename Func>
int
dispatch_backend(const char* backend, Func&& func)
{
    namespace corosio = boost::corosio;

#if BOOST_COROSIO_HAS_IO_URING
    if (std::strcmp(backend, "io_uring") == 0)
    {
        func(
            []() -> std::unique_ptr<corosio::io_context> {
                return std::make_unique<corosio::io_context>(corosio::io_uring);
            },
            corosio::io_uring, "io_uring");
        return 0;
    }
#endif

#if BOOST_COROSIO_HAS_EPOLL
    if (std::strcmp(backend, "epoll") == 0)
    {
        func(
            []() -> std::unique_ptr<corosio::io_context> {
                return std::make_unique<corosio::io_context>(corosio::epoll);
            },
            corosio::epoll, "epoll");
        return 0;
    }
#endif

#if BOOST_COROSIO_HAS_KQUEUE
    if (std::strcmp(backend, "kqueue") == 0)
    {
        func(
            []() -> std::unique_ptr<corosio::io_context> {
                return std::make_unique<corosio::io_context>(corosio::kqueue);
            },
            corosio::kqueue, "kqueue");
        return 0;
    }
#endif

#if BOOST_COROSIO_HAS_SELECT
    if (std::strcmp(backend, "select") == 0)
    {
        func(
            []() -> std::unique_ptr<corosio::io_context> {
                return std::make_unique<corosio::io_context>(corosio::select);
            },
            corosio::select, "select");
        return 0;
    }
#endif

#if BOOST_COROSIO_HAS_IOCP
    if (std::strcmp(backend, "iocp") == 0)
    {
        func(
            []() -> std::unique_ptr<corosio::io_context> {
                return std::make_unique<corosio::io_context>(corosio::iocp);
            },
            corosio::iocp, "iocp");
        return 0;
    }
#endif

    std::cerr << "Error: Backend '" << backend
              << "' is not available on this platform.\n\n";
    print_available_backends();
    return 1;
}

} // namespace perf

#endif // BOOST_COROSIO_PERF_BACKEND_SELECTION_HPP
