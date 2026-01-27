//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_OVERLAPPED_OP_HPP
#define BOOST_COROSIO_DETAIL_IOCP_OVERLAPPED_OP_HPP


#if defined(_WIN32)

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/make_err.hpp"
#include "src/detail/scheduler_op.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <stop_token>

#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

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

    capy::coro h;
    capy::executor_ref d;
    system::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;
    DWORD dwError = 0;
    DWORD bytes_transferred = 0;
    bool empty_buffer = false;  // True if operation was with empty buffer
    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    // Synchronizes GQCS completion with initiating function return.
    // GQCS can complete before WSARecv/etc returns; ready_=1 means
    // the initiator is done and the op can be dispatched.
    long ready_ = 0;

    overlapped_op()
    {
        data_ = this;
    }

    void reset() noexcept
    {
        Internal = 0;
        InternalHigh = 0;
        Offset = 0;
        OffsetHigh = 0;
        hEvent = nullptr;
        dwError = 0;
        bytes_transferred = 0;
        empty_buffer = false;
        cancelled.store(false, std::memory_order_relaxed);
        ready_ = 0;
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
            {
                // Explicit cancellation via cancel() or stop_token
                *ec_out = capy::error::canceled;
            }
            else if (dwError != 0)
            {
                *ec_out = make_err(dwError);
            }
            else if (is_read_operation() && bytes_transferred == 0 && !empty_buffer)
            {
                // EOF: 0 bytes transferred with no error indicates end of stream
                // (but not if we intentionally read with an empty buffer)
                *ec_out = capy::error::eof;
            }
        }

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        d.dispatch(h).resume();
    }

    // Returns true if this is a read operation (for EOF detection)
    virtual bool is_read_operation() const noexcept { return false; }

    void destroy() override
    {
        stop_cb.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    /** Hook for derived classes to perform actual I/O cancellation. */
    virtual void do_cancel() noexcept
    {
    }

    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void complete(DWORD bytes, DWORD err) noexcept
    {
        bytes_transferred = bytes;
        dwError = err;
    }
};

inline overlapped_op*
get_overlapped_op(scheduler_op* h) noexcept
{
    return static_cast<overlapped_op*>(h->data());
}

} // namespace boost::corosio::detail

#endif // _WIN32

#endif // BOOST_COROSIO_DETAIL_IOCP_OVERLAPPED_OP_HPP
