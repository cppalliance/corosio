//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/kqueue_context.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/scheduler.hpp"
#include "src/detail/kqueue/sockets.hpp"
#include "src/detail/kqueue/acceptors.hpp"

#include <algorithm>
#include <thread>

/*
    kqueue_context owns the lifecycle of all kqueue-based I/O services.
    Construction creates the kqueue_scheduler first (passing the concurrency
    hint), then registers kqueue_socket_service and kqueue_acceptor_service.
    Those services are keyed by their base classes (socket_service /
    acceptor_service), so higher-level code discovers them through
    execution_context::use_service without knowing the kqueue concrete type.
    The scheduler must outlive both services because they post completions
    and track outstanding work through it.
*/

namespace boost::corosio {

kqueue_context::
kqueue_context()
    : kqueue_context(std::max(std::thread::hardware_concurrency(), 1u))
{
}

kqueue_context::
kqueue_context(
    unsigned concurrency_hint)
{
    sched_ = &make_service<detail::kqueue_scheduler>(
        static_cast<int>(concurrency_hint));

    make_service<detail::kqueue_socket_service>();
    make_service<detail::kqueue_acceptor_service>();
}

kqueue_context::
~kqueue_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_KQUEUE
