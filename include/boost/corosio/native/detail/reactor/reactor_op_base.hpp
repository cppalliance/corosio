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

#include <boost/corosio/detail/scheduler_op.hpp>

#include <atomic>
#include <cstddef>
#include <memory>

namespace boost::corosio::detail {

/** Non-template base for reactor operations.

    Holds per-operation state accessed by reactor_descriptor_state
    and reactor_socket without requiring knowledge of the concrete
    backend socket/acceptor types. This avoids duplicate template
    instantiations for the descriptor_state and scheduler hot paths.

    @see reactor_op
*/
struct reactor_op_base : scheduler_op
{
    /// Errno from the last I/O attempt.
    int errn = 0;

    /// Bytes transferred on success.
    std::size_t bytes_transferred = 0;

    /// True when cancellation has been requested.
    std::atomic<bool> cancelled{false};

    /// Prevents use-after-free when socket is closed with pending ops.
    std::shared_ptr<void> impl_ptr;

    /// Record the result of an I/O attempt.
    void complete(int err, std::size_t bytes) noexcept
    {
        errn              = err;
        bytes_transferred = bytes;
    }

    /// Perform the I/O syscall (overridden by concrete op types).
    virtual void perform_io() noexcept {}

    /// Mark as cancelled (visible to the I/O completion path).
    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    /// Destroy without invoking.
    void destroy() override
    {
        impl_ptr.reset();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_BASE_HPP
