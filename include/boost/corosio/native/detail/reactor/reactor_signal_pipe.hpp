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

    On each read-readiness edge the drain op completes without touching the
    pipe, and invoke_deferred_io runs it once the descriptor lock is
    released. Only then does it drain the pipe and deliver the signals:
    delivery posts into every context watching the pipe, and a post made
    under this descriptor's lock would order it before another context's
    scheduler lock, the reverse of that scheduler's own order. The op then
    parks itself again, draining once more if an edge arrived meanwhile.
*/

namespace boost::corosio::detail {

struct reactor_signal_pipe_reader
{
    struct drain_op final : reactor_op_base
    {
        reactor_signal_pipe_reader* reader = nullptr;

        void perform_io() noexcept override
        {
            errn = 0;
        }

        void operator()() override
        {
            auto& desc = reader->desc;
            for (;;)
            {
                posix_signal_detail::drain_signal_pipe();
                conditionally_enabled_mutex::scoped_lock lock(desc.mutex);
                // An edge seen while unparked set read_ready instead.
                if (!desc.read_ready)
                {
                    desc.read_op = this;
                    break;
                }
                desc.read_ready = false;
            }
            // A select() started while this op was out ran without the
            // pipe in its read set.
            desc.scheduler_->notify_reparked();
            // Nothing counted this op as work; balance the decrement
            // that follows the inline run of a completed op.
            desc.scheduler_->compensating_work_started();
        }

        void destroy() override {}
    };

    reactor_descriptor_state desc;
    drain_op op;

    // Park the drain op and return the descriptor to hand to
    // scheduler::register_descriptor(read_fd, ...).
    reactor_descriptor_state* arm() noexcept
    {
        op.reader    = this;
        desc.read_op = &op;
        return &desc;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SIGNAL_PIPE_HPP
