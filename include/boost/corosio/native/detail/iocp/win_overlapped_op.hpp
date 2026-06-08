//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OVERLAPPED_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OVERLAPPED_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/error.hpp>
#include <system_error>

#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/coro_op.hpp>
#include <boost/corosio/native/detail/coro_op_complete.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** Base class for IOCP overlapped operations.

    Derives from both OVERLAPPED (for Windows IOCP) and scheduler_op
    (for queueing). Uses function pointer dispatch inherited from
    scheduler_op - no virtual functions.

    The OVERLAPPED structure is at the start so we can static_cast
    between OVERLAPPED* and overlapped_op*.
*/
struct overlapped_op
    : OVERLAPPED
    , coro_op
{
    /** Function pointer type for cancellation hook. */
    using cancel_func_type = void (*)(overlapped_op*) noexcept;

    long ready_ = 0;
    DWORD dwError           = 0;
    DWORD bytes_transferred = 0;
    cancel_func_type cancel_func_ = nullptr;

    explicit overlapped_op(func_type func) noexcept : coro_op(func)
    {
        reset_overlapped();
    }

    void reset_overlapped() noexcept
    {
        Internal     = 0;
        InternalHigh = 0;
        Offset       = 0;
        OffsetHigh   = 0;
        hEvent       = nullptr;
    }

    void reset() noexcept
    {
        reset_overlapped();
        ready_            = 0;
        dwError           = 0;
        bytes_transferred = 0;
        empty_buffer      = false;
        is_read           = false;
        cancelled.store(false, std::memory_order_relaxed);
    }

    // coro_op::request_cancel() (set the cancelled flag) is inherited
    // and used directly by close()/cancel() paths. The stop_token path
    // additionally drives the kernel via on_cancel() below.

    void do_cancel() noexcept
    {
        if (cancel_func_)
            cancel_func_(this);
    }

    /** IOCP cancellation hook (stop_token path): set the flag, then issue
        the registered CancelIoEx / wait-reactor deregister via cancel_func_. */
    void on_cancel() noexcept override
    {
        request_cancel();
        do_cancel();
    }

    void store_result(DWORD bytes, DWORD err) noexcept
    {
        bytes_transferred = bytes;
        dwError           = err;
    }

    /** Write results to output parameters and resume coroutine. */
    void invoke_handler()
    {
        stop_cb.reset();

        decode_io_result(
            ec_out,
            cancelled.load(std::memory_order_acquire),
            dwError != 0 ? make_err(dwError) : std::error_code{},
            is_read, static_cast<std::size_t>(bytes_transferred),
            empty_buffer);

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        cont_op.cont.h = h;
        dispatch_coro(ex, cont_op.cont).resume();
    }

    /** Disarm cancellation and abandon the coroutine handle. */
    void cleanup_only()
    {
        stop_cb.reset();
        h = {};
    }
};

/** Cast OVERLAPPED* to overlapped_op*.

    Safe because overlapped_op has OVERLAPPED as first base class.
*/
inline overlapped_op*
overlapped_to_op(LPOVERLAPPED ov) noexcept
{
    return static_cast<overlapped_op*>(ov);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_OVERLAPPED_OP_HPP
