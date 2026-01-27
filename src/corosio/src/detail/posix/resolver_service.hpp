//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_RESOLVER_SERVICE_HPP
#define BOOST_COROSIO_DETAIL_POSIX_RESOLVER_SERVICE_HPP

#include "src/detail/config_backend.hpp"

// This implementation works for all POSIX backends (epoll, kqueue, io_uring, poll)
#if !defined(BOOST_COROSIO_BACKEND_IOCP)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/capy/ex/execution_context.hpp>

/*
    POSIX Resolver Service
    ======================

    POSIX getaddrinfo() is a blocking call that cannot be monitored with
    epoll/kqueue/io_uring. We use a worker thread approach: each resolution
    spawns a dedicated thread that runs the blocking call and posts completion
    back to the scheduler.

    This follows the timer_service pattern:
    - posix_resolver_service is an abstract base class (no scheduler dependency)
    - posix_resolver_service_impl is the concrete implementation
    - get_resolver_service(ctx, sched) creates the service with scheduler ref

    Thread-per-resolution Design
    ----------------------------
    Simple, no thread pool complexity. DNS lookups are infrequent enough that
    thread creation overhead is acceptable. Detached threads self-manage;
    shared_ptr capture keeps impl alive until completion.

    Cancellation
    ------------
    getaddrinfo() cannot be interrupted mid-call. We use an atomic flag to
    indicate cancellation was requested. The worker thread checks this flag
    after getaddrinfo() returns and reports the appropriate error.
*/

namespace boost::corosio::detail {

struct scheduler;

//------------------------------------------------------------------------------

/** Abstract resolver service for POSIX backends.

    This is the base class that defines the interface. The concrete
    implementation (posix_resolver_service_impl) is created via
    get_resolver_service() which passes the scheduler reference.
*/
class posix_resolver_service : public capy::execution_context::service
{
public:
    /** Create a new resolver implementation. */
    virtual resolver::resolver_impl& create_impl() = 0;

protected:
    posix_resolver_service() = default;
};

//------------------------------------------------------------------------------

/** Get or create the resolver service for the given context.

    This function is called by the concrete scheduler during initialization
    to create the resolver service with a reference to itself.

    @param ctx Reference to the owning execution_context.
    @param sched Reference to the scheduler for posting completions.
    @return Reference to the resolver service.
*/
posix_resolver_service&
get_resolver_service(capy::execution_context& ctx, scheduler& sched);

} // namespace boost::corosio::detail

#endif // !BOOST_COROSIO_BACKEND_IOCP

#endif // BOOST_COROSIO_DETAIL_POSIX_RESOLVER_SERVICE_HPP
