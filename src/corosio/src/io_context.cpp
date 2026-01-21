//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)
#include "src/detail/iocp/scheduler.hpp"
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/scheduler.hpp"
#elif defined(BOOST_COROSIO_BACKEND_KQUEUE)
#include "src/detail/kqueue/scheduler.hpp"
#endif

#include <thread>

namespace boost {
namespace corosio {

#if defined(BOOST_COROSIO_BACKEND_IOCP)
using scheduler_type = detail::win_scheduler;
#elif defined(BOOST_COROSIO_BACKEND_EPOLL)
using scheduler_type = detail::epoll_scheduler;
#elif defined(BOOST_COROSIO_BACKEND_KQUEUE)
using scheduler_type = detail::kqueue_scheduler;
#endif

io_context::
io_context()
    : io_context(std::thread::hardware_concurrency())
{
}

io_context::
io_context(
    unsigned concurrency_hint)
    : sched_(make_service<scheduler_type>(concurrency_hint))
{
}

} // namespace corosio
} // namespace boost
