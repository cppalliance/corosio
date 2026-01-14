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
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>
#include <boost/capy/concept/affine_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_list.hpp>

#include "detail/windows.hpp"
#include "detail/win_overlapped_op.hpp"
#include "detail/win_mutex.hpp"
#include "detail/win_wsa_init.hpp"

#include <MSWSock.h>
#include <Ws2tcpip.h>

namespace boost {
namespace corosio {
namespace detail {

class win_iocp_scheduler;
class win_iocp_sockets;
class win_socket_impl;
class win_acceptor_impl;

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

    bool is_read_operation() const noexcept override { return true; }
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
    win_socket_impl* peer_impl = nullptr;  // New impl for accepted socket
    SOCKET listen_socket = INVALID_SOCKET;  // For SO_UPDATE_ACCEPT_CONTEXT
    io_object::io_object_impl** impl_out = nullptr;  // Output: impl for awaitable
    // Buffer for AcceptEx: local + remote addresses
    char addr_buf[2 * (sizeof(sockaddr_in6) + 16)];

    /** Resume the coroutine after accept completes. */
    void operator()() override;
};

//------------------------------------------------------------------------------

/** Socket implementation for IOCP-based I/O.

    This class contains the state for a single socket, including
    the native socket handle and pending operations. It derives from
    intrusive_list::node to allow tracking by the win_iocp_sockets service.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_socket_impl
    : public socket::socket_impl
    , public capy::intrusive_list<win_socket_impl>::node
{
    friend class win_iocp_sockets;

public:
    explicit win_socket_impl(win_iocp_sockets& svc) noexcept;

    void release() override;

    void connect(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        endpoint,
        std::stop_token,
        system::error_code*) override;

    void read_some(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        any_bufref&,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    void write_some(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        any_bufref&,
        std::stop_token,
        system::error_code*,
        std::size_t*) override;

    SOCKET native_handle() const noexcept { return socket_; }
    bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_socket(SOCKET s) noexcept { socket_ = s; }

    connect_op conn_;
    read_op rd_;
    write_op wr_;

private:
    win_iocp_sockets& svc_;
    SOCKET socket_ = INVALID_SOCKET;
};

//------------------------------------------------------------------------------

/** Acceptor implementation for IOCP-based I/O.

    This class contains the state for a listening socket, including
    the native socket handle and pending accept operation.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_acceptor_impl
    : public acceptor::acceptor_impl
    , public capy::intrusive_list<win_acceptor_impl>::node
{
    friend class win_iocp_sockets;

public:
    explicit win_acceptor_impl(win_iocp_sockets& svc) noexcept;

    void release() override;

    void accept(
        std::coroutine_handle<>,
        capy::any_dispatcher,
        std::stop_token,
        system::error_code*,
        io_object::io_object_impl**) override;

    SOCKET native_handle() const noexcept { return socket_; }
    bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }
    void cancel() noexcept;
    void close_socket() noexcept;

    accept_op acc_;

private:
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
    : private win_wsa_init
    , public capy::execution_context::service
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
    win_socket_impl& create_impl();

    /** Destroy a socket implementation. */
    void destroy_impl(win_socket_impl& impl);

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation to initialize.
        @return Error code, or success.
    */
    system::error_code open_socket(win_socket_impl& impl);

    /** Create a new acceptor implementation. */
    win_acceptor_impl& create_acceptor_impl();

    /** Destroy an acceptor implementation. */
    void destroy_acceptor_impl(win_acceptor_impl& impl);

    /** Create, bind, and listen on an acceptor socket.

        @param impl The acceptor implementation to initialize.
        @param ep The local endpoint to bind to.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    system::error_code open_acceptor(
        win_acceptor_impl& impl,
        endpoint ep,
        int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept { return iocp_; }

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept { return connect_ex_; }

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept { return accept_ex_; }

    /** Post an overlapped operation for completion. */
    void post(overlapped_op* op);

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

private:
    void load_extension_functions();

    win_iocp_scheduler& sched_;
    win_mutex mutex_;
    capy::intrusive_list<win_socket_impl> socket_list_;
    capy::intrusive_list<win_acceptor_impl> acceptor_list_;
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
