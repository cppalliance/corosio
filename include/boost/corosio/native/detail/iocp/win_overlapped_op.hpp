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

/** Convert an IOCP completion's raw error to std::error_code, disambiguating
    ERROR_NETNAME_DELETED by operation kind and surfacing the remote-connection
    error family as portable std::errc conditions.

    IOCP delivers ERROR_NETNAME_DELETED both when a local closesocket() cancels
    a pending op (already handled by the cancelled flag before this is reached)
    and when the peer hard-closes with a RST. A NETNAME_DELETED that survives
    the cancelled check is therefore a genuine remote reset, and the correct
    surface depends on the operation: connection_reset for stream read/write,
    connection_aborted for accept (mirroring Asio's per-op mapping).

    Why generic_category / std::errc rather than a native code: which raw
    Windows code std::system_category maps to a given std::errc condition
    depends on the standard library. MSVC's STL maps the WSA-range socket
    codes (WSAECONNRESET 10054, WSAECONNREFUSED 10061, ...) to the std::errc
    conditions but not their 12xx Win32 counterparts; libstdc++ (MinGW) does
    the exact opposite -- it maps the Win32 12xx codes but not the WSA range.
    So no single system_category code compares equal to e.g.
    std::errc::connection_refused on both toolchains. Returning the condition
    itself via std::make_error_code (generic_category) sidesteps the
    divergence: it compares equal to the matching std::errc on every standard
    library, mirroring what the POSIX backends yield from errno. The trade-off
    is a generic (less Windows-specific) message string.

    Both the WSA and Win32 forms are accepted because the delivered form
    varies by operation: a WSASend/WSARecv completion surfaces the Winsock
    code directly (e.g. WSAECONNRESET 10054), whereas a ConnectEx failure is
    completed with an NTSTATUS and GetQueuedCompletionStatus reports the Win32
    code RtlNtStatusToDosError derives from it (e.g. ERROR_CONNECTION_REFUSED
    1225). This normalization lives here, in the IOCP layer, rather than in
    the platform-neutral make_err. All other codes defer to make_err.

    @param dwError     The Windows error code (DWORD).
    @param accept_path True on the accept completion path.
    @return The corresponding std::error_code.
*/
inline std::error_code
iocp_make_err(DWORD dwError, bool accept_path) noexcept
{
    // A pending op hit by a remote RST completes with ERROR_NETNAME_DELETED;
    // its portable meaning depends on the operation (reset vs aborted).
    if (dwError == ERROR_NETNAME_DELETED)
        return std::make_error_code(accept_path
            ? std::errc::connection_aborted
            : std::errc::connection_reset);

    switch (dwError)
    {
    case WSAECONNRESET:                                    // 10054
        return std::make_error_code(std::errc::connection_reset);
    case WSAECONNREFUSED: case ERROR_CONNECTION_REFUSED:   // 10061 / 1225
        return std::make_error_code(std::errc::connection_refused);
    case WSAECONNABORTED: case ERROR_CONNECTION_ABORTED:   // 10053 / 1236
        return std::make_error_code(std::errc::connection_aborted);
    case WSAENETUNREACH:  case ERROR_NETWORK_UNREACHABLE:  // 10051 / 1231
        return std::make_error_code(std::errc::network_unreachable);
    case WSAEHOSTUNREACH: case ERROR_HOST_UNREACHABLE:     // 10065 / 1232
        return std::make_error_code(std::errc::host_unreachable);
    case WSAETIMEDOUT:    case ERROR_SEM_TIMEOUT:          // 10060 / 121
        return std::make_error_code(std::errc::timed_out);
    // Closed-object contract: MSVC maps ERROR_INVALID_HANDLE to
    // invalid_argument and MinGW's WSAEBADF mapping is unreliable, so
    // normalize both spellings of "dead handle" here.
    case WSAEBADF:        case ERROR_INVALID_HANDLE:       // 10009 / 6
        return std::make_error_code(std::errc::bad_file_descriptor);
    default:
        break;
    }
    return make_err(dwError);
}

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

    /** Completion handshake between the I/O initiator and the GQCS completer,
        and the release/acquire barrier that publishes the payload: the plain
        `dwError` / `bytes_transferred` fields are written before a release
        store to `ready_` and read after an acquiring load, so they need no
        atomicity of their own. The CAS protocol lives at the access sites in
        win_scheduler.hpp. */
    std::atomic<long> ready_{0};
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
        ready_.store(0, std::memory_order_relaxed);
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
            dwError != 0 ? iocp_make_err(dwError, /*accept_path=*/false)
                         : std::error_code{},
            is_read, static_cast<std::size_t>(bytes_transferred),
            empty_buffer);

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        cont.h = h;
        dispatch_coro(ex, cont).resume();
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
