//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/epoll_context.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include "src/detail/epoll/scheduler.hpp"
#include "src/detail/epoll/sockets.hpp"
#include "src/detail/epoll/acceptors.hpp"

#include <thread>

namespace boost::corosio {

epoll_context::
epoll_context()
    : epoll_context(std::thread::hardware_concurrency())
{
}

epoll_context::
epoll_context(
    unsigned concurrency_hint)
{
    sched_ = &make_service<detail::epoll_scheduler>(
        static_cast<int>(concurrency_hint));

    // Install socket/acceptor services.
    // These use socket_service and acceptor_service as key_type,
    // enabling runtime polymorphism.
    make_service<detail::epoll_socket_service>();
    make_service<detail::epoll_acceptor_service>();
}

epoll_context::
~epoll_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_EPOLL
