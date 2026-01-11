//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_OVERLAPPED_OP_HPP
#define BOOST_COROSIO_DETAIL_WIN_OVERLAPPED_OP_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <Windows.h>

#include <boost/capy/affine.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstddef>
#include <optional>
#include <stop_token>

namespace boost {
namespace corosio {
namespace detail {

struct overlapped_op
    : OVERLAPPED
    , capy::execution_context::handler  // handler already has intrusive_list node
{
    struct canceller
    {
        overlapped_op* op;
        void operator()() const noexcept { op->request_cancel(); }
    };

    capy::coro h;
    capy::any_dispatcher d;
    system::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;
    DWORD error = 0;
    DWORD bytes_transferred = 0;
    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    // Synchronizes GQCS completion with initiating function return.
    // GQCS can complete before WSARecv/etc returns; ready_=1 means
    // the initiator is done and the op can be dispatched.
    long ready_ = 0;

    void reset() noexcept
    {
        Internal = 0;
        InternalHigh = 0;
        Offset = 0;
        OffsetHigh = 0;
        hEvent = nullptr;
        error = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        ready_ = 0;
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = make_error_code(system::errc::operation_canceled);
            else if (error != 0)
                *ec_out = system::error_code(
                    static_cast<int>(error), system::system_category());
        }

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        d(h).resume();
    }

    void destroy() override
    {
        stop_cb.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
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
        error = err;
    }
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
