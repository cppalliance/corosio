//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
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
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/error.hpp>
#include <system_error>

#include <boost/corosio/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <stop_token>

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
    , scheduler_op
{
    struct canceller
    {
        overlapped_op* op;
        void operator()() const noexcept
        {
            op->request_cancel();
            op->do_cancel();
        }
    };

    /** Function pointer type for cancellation hook. */
    using cancel_func_type = void (*)(overlapped_op*) noexcept;

    long ready_             = 0;
    std::coroutine_handle<> h;
    capy::executor_ref ex;
    std::error_code* ec_out = nullptr;
    std::size_t* bytes_out  = nullptr;
    DWORD dwError           = 0;
    DWORD bytes_transferred = 0;
    bool empty_buffer       = false;
    bool is_read_           = false;
    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;
    cancel_func_type cancel_func_ = nullptr;

    explicit overlapped_op(func_type func) noexcept : scheduler_op(func)
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
        is_read_          = false;
        cancelled.store(false, std::memory_order_relaxed);
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void do_cancel() noexcept
    {
        if (cancel_func_)
            cancel_func_(this);
    }

    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
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

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = capy::error::canceled;
            else if (dwError != 0)
                *ec_out = make_err(dwError);
            else if (is_read_ && bytes_transferred == 0 && !empty_buffer)
                *ec_out = capy::error::eof;
            else
                *ec_out = {};
        }

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        dispatch_coro(ex, h).resume();
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
