//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/iocp_context.hpp>

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/scheduler.hpp"

#include <thread>

namespace boost {
namespace corosio {

iocp_context::
iocp_context()
    : iocp_context(std::thread::hardware_concurrency())
{
}

iocp_context::
iocp_context(
    unsigned concurrency_hint)
{
    sched_ = &make_service<detail::win_scheduler>(
        static_cast<int>(concurrency_hint));
}

iocp_context::
~iocp_context()
{
    shutdown();
    destroy();
}

} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_IOCP
