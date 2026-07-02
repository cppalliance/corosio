//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SIGNAL_PIPE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SIGNAL_PIPE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>

#include <errno.h>

/*
    Reactor signal-pipe reader
    ==========================

    Bridges the global POSIX signal self-pipe (see posix_signal_service.hpp)
    to the reactor backends (epoll/kqueue/select). The concrete scheduler owns
    one of these for its lifetime and, in register_signal_reader(), parks the
    drain op as the descriptor's read_op then calls register_descriptor().

    The drain op never completes: on each read-readiness edge, invoke_deferred_io
    calls perform_io(), which drains the pipe and delivers the pending signals,
    then reports EAGAIN so the op stays parked and re-fires on the next edge —
    exactly the path a socket read op takes when the kernel has no more data.
    Because deliver_signal() runs here, in normal dispatch context (not in the
    signal handler and not under the reactor poll lock), its mutex locking is
    safe.
*/

namespace boost::corosio::detail {

struct reactor_signal_pipe_reader
{
    // Parked read op: drains the self-pipe and re-arms via EAGAIN.
    struct drain_op final : reactor_op_base
    {
        void perform_io() noexcept override
        {
            posix_signal_detail::drain_signal_pipe();
            // Stay parked: EAGAIN tells invoke_deferred_io to keep this op
            // installed as read_op and re-run it on the next readiness edge.
            errn = EAGAIN;
        }
    };

    reactor_descriptor_state desc;
    drain_op                 op;

    // Park the drain op and return the descriptor to hand to
    // scheduler::register_descriptor(read_fd, ...).
    reactor_descriptor_state* arm() noexcept
    {
        desc.read_op = &op;
        return &desc;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SIGNAL_PIPE_HPP
