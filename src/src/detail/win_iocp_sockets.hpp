//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_IOCP_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_WIN_IOCP_SOCKETS_HPP

#include <boost/corosio/detail/config.hpp>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <Windows.h>
#include <MSWSock.h>

#include <boost/capy/affine.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/execution_context.hpp>
#include <boost/capy/executor.hpp>
#include <boost/capy/intrusive_list.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <system_error>

namespace boost {
namespace corosio {
namespace detail {

class win_iocp_sockets;
class socket_impl;

//------------------------------------------------------------------------------

/** Base class for all IOCP overlapped operations.

    This class inherits from both OVERLAPPED (for Windows I/O) and
    executor_work (for coroutine dispatch). It provides common
    functionality for all async socket operations.

    @note The OVERLAPPED must be first in memory layout for proper
    casting when receiving completions from IOCP.
*/
struct overlapped_op
    : OVERLAPPED
    , capy::executor_work
{
    /** Small invocable for stop_callback - avoids std::function overhead. */
    struct canceller
    {
        overlapped_op* op;
        void operator()() const noexcept { op->request_cancel(); }
    };

    capy::coro h;
    capy::any_dispatcher d;
    std::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;
    DWORD error = 0;
    DWORD bytes_transferred = 0;
    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    /** Initialize the OVERLAPPED structure. */
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
    }

    /** Resume the coroutine via its dispatcher. */
    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = std::make_error_code(std::errc::operation_canceled);
            else if (error != 0)
                *ec_out = std::error_code(
                    static_cast<int>(error), std::system_category());
        }

        if (bytes_out)
            *bytes_out = static_cast<std::size_t>(bytes_transferred);

        d(h).resume();
    }

    /** Destroy - no-op since we're owned by socket_impl. */
    void destroy() override
    {
        stop_cb.reset();
    }

    /** Request cancellation of this operation. */
    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    /** Start tracking with a stop token. */
    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    /** Complete the operation with results from IOCP. */
    void complete(DWORD bytes, DWORD err) noexcept
    {
        bytes_transferred = bytes;
        error = err;
    }
};

//------------------------------------------------------------------------------

/** Connect operation state. */
struct connect_op : overlapped_op
{
};

/** Read operation state with buffer descriptors. */
struct read_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags = 0;
};

/** Write operation state with buffer descriptors. */
struct write_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
};

/** Accept operation state. */
struct accept_op : overlapped_op
{
    SOCKET accepted_socket = INVALID_SOCKET;
    socket_impl* peer_impl = nullptr;  // New impl for accepted socket
    void* peer_socket = nullptr;  // Pointer to peer socket object
    void* sockets_svc = nullptr;  // Pointer to win_iocp_sockets service
    SOCKET listen_socket = INVALID_SOCKET;  // For SO_UPDATE_ACCEPT_CONTEXT
    // Buffer for AcceptEx: local + remote addresses
    char addr_buf[2 * (sizeof(sockaddr_in6) + 16)];

    // Transfer callback - set by acceptor
    void (*transfer_fn)(void* peer, void* svc, socket_impl* impl, SOCKET sock) = nullptr;

    /** Resume the coroutine, transferring the accepted socket. */
    void operator()() override;
};

//------------------------------------------------------------------------------

/** Socket implementation for IOCP-based I/O.

    This class contains the state for a single socket, including
    the native socket handle and pending operations. It derives from
    intrusive_list::node to allow tracking by the win_iocp_sockets service.

    @note Internal implementation detail. Users interact with socket class.
*/
class socket_impl
    : public capy::intrusive_list<socket_impl>::node
{
    friend class win_iocp_sockets;

public:
    /** Construct a socket implementation.

        @param svc Reference to the owning sockets service.
    */
    explicit socket_impl(win_iocp_sockets& svc) noexcept
        : svc_(svc)
    {
    }

    /** Return the native socket handle. */
    SOCKET native_handle() const noexcept { return socket_; }

    /** Check if the socket has a valid handle. */
    bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }

    /** Cancel all pending operations on this socket. */
    void cancel() noexcept;

    /** Close the native socket handle. */
    void close_socket() noexcept;

    /** Release this implementation back to the service. */
    void release();

    connect_op conn_;
    read_op rd_;
    write_op wr_;
    accept_op acc_;

    /** Set the native socket handle.
    
        Used by acceptor to transfer accepted sockets.
    */
    void set_socket(SOCKET s) noexcept { socket_ = s; }

private:
    friend class win_iocp_sockets;
    win_iocp_sockets& svc_;
    SOCKET socket_ = INVALID_SOCKET;
};

//------------------------------------------------------------------------------

/** Windows IOCP socket management service.

    This service owns all socket implementations and coordinates their
    lifecycle with the IOCP. It provides:

    - Socket implementation allocation and deallocation
    - IOCP handle association for sockets
    - Function pointer loading for ConnectEx/AcceptEx
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class win_iocp_sockets
    : public capy::execution_context::service
{
public:
    using key_type = win_iocp_sockets;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_iocp_sockets(capy::execution_context& ctx);

    /** Destroy the socket service. */
    ~win_iocp_sockets();

    win_iocp_sockets(win_iocp_sockets const&) = delete;
    win_iocp_sockets& operator=(win_iocp_sockets const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new socket implementation. */
    socket_impl& create_impl();

    /** Destroy a socket implementation. */
    void destroy_impl(socket_impl& impl);

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation to initialize.
        @return Error code, or success.
    */
    std::error_code open_socket(socket_impl& impl);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept { return iocp_; }

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept { return connect_ex_; }

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept { return accept_ex_; }

private:
    void load_extension_functions();

    std::mutex mutex_;
    capy::intrusive_list<socket_impl> list_;
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32

#endif
