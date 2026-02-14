//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/iocp_context.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/iocp/sockets.hpp"
#include "src/detail/iocp/signals.hpp"

#include <thread>

namespace boost::corosio {

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

    auto& sockets = make_service<detail::win_sockets>();
    make_service<detail::win_acceptor_service>(sockets);
    make_service<detail::win_signals>();
}

iocp_context::
~iocp_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
