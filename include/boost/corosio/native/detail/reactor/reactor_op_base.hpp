//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_BASE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_BASE_HPP

#include <boost/corosio/native/detail/coro_op.hpp>

#include <cstddef>

namespace boost::corosio::detail {

/** Non-template base for reactor operations.

    Adds the reactor-specific result model (the recorded errno + bytes and
    the perform_io() readiness re-run) on top of coro_op, the shared op
    envelope (coroutine handle, executor, output pointers, stop_token wiring,
    cancelled flag, impl_ptr keepalive) common to every backend.

    Kept as a non-template layer so reactor_descriptor_state and the
    scheduler hot paths can touch op state without knowing the concrete
    socket/acceptor types.

    @see coro_op, reactor_op
*/
struct reactor_op_base : coro_op
{
    /// Errno from the last I/O attempt.
    int errn = 0;

    /// Bytes transferred on success.
    std::size_t bytes_transferred = 0;

    // cancelled, impl_ptr, and request_cancel() are inherited from coro_op.

    /// Record the result of an I/O attempt.
    void complete(int err, std::size_t bytes) noexcept
    {
        errn              = err;
        bytes_transferred = bytes;
    }

    /// Perform the I/O syscall (overridden by concrete op types).
    virtual void perform_io() noexcept {}

    /// Destroy without invoking — drop the keepalive (impl_ptr from coro_op).
    void destroy() override
    {
        impl_ptr.reset();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_BASE_HPP
