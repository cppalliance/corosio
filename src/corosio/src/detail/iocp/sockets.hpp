//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_SOCKETS_HPP
#define BOOST_COROSIO_DETAIL_IOCP_SOCKETS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/corosio/socket.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"

#include "src/detail/iocp/windows.hpp"
#include "src/detail/iocp/completion_key.hpp"
#include "src/detail/iocp/overlapped_op.hpp"
#include "src/detail/iocp/mutex.hpp"
#include "src/detail/iocp/wsa_init.hpp"

#include <memory>

#include <MSWSock.h>
#include <Ws2tcpip.h>

namespace boost {
namespace corosio {
namespace detail {

class win_scheduler;
class win_sockets;
class win_socket_impl;
class win_socket_impl_internal;
class win_acceptor_impl;
class win_acceptor_impl_internal;

//------------------------------------------------------------------------------

/** Connect operation state. */
struct connect_op : overlapped_op
{
    win_socket_impl_internal& internal;
    std::shared_ptr<win_socket_impl_internal> internal_ptr;  // Keeps internal alive during I/O

    explicit connect_op(win_socket_impl_internal& internal_) noexcept : internal(internal_) {}

    void operator()() override;
    void do_cancel() noexcept override;
};

/** Read operation state with buffer descriptors. */
struct read_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    DWORD flags = 0;
    win_socket_impl_internal& internal;
    std::shared_ptr<win_socket_impl_internal> internal_ptr;  // Keeps internal alive during I/O

    explicit read_op(win_socket_impl_internal& internal_) noexcept : internal(internal_) {}

    void operator()() override;
    bool is_read_operation() const noexcept override { return true; }
    void do_cancel() noexcept override;
};

/** Write operation state with buffer descriptors. */
struct write_op : overlapped_op
{
    static constexpr std::size_t max_buffers = 16;
    WSABUF wsabufs[max_buffers];
    DWORD wsabuf_count = 0;
    win_socket_impl_internal& internal;
    std::shared_ptr<win_socket_impl_internal> internal_ptr;  // Keeps internal alive during I/O

    explicit write_op(win_socket_impl_internal& internal_) noexcept : internal(internal_) {}

    void operator()() override;
    void do_cancel() noexcept override;
};

/** Accept operation state. */
struct accept_op : overlapped_op
{
    SOCKET accepted_socket = INVALID_SOCKET;
    win_socket_impl* peer_wrapper = nullptr;  // Wrapper for accepted socket
    std::shared_ptr<win_acceptor_impl_internal> acceptor_ptr;  // Keeps acceptor alive during I/O
    SOCKET listen_socket = INVALID_SOCKET;  // For SO_UPDATE_ACCEPT_CONTEXT
    io_object::io_object_impl** impl_out = nullptr;  // Output: wrapper for awaitable
    // Buffer for AcceptEx: local + remote addresses
    char addr_buf[2 * (sizeof(sockaddr_in6) + 16)];

    /** Resume the coroutine after accept completes. */
    void operator()() override;

    /** Cancel the pending accept via CancelIoEx. */
    void do_cancel() noexcept override;
};

//------------------------------------------------------------------------------

/** Internal socket state for IOCP-based I/O.

    This class contains the actual state for a single socket, including
    the native socket handle and pending operations. It derives from
    enable_shared_from_this so operations can extend its lifetime.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_socket_impl_internal
    : public intrusive_list<win_socket_impl_internal>::node
    , public std::enable_shared_from_this<win_socket_impl_internal>
{
    friend class win_sockets;
    friend class win_socket_impl;
    friend struct read_op;
    friend struct write_op;
    friend struct connect_op;

    win_sockets& svc_;
    connect_op conn_;
    read_op rd_;
    write_op wr_;
    SOCKET socket_ = INVALID_SOCKET;

public:
    explicit win_socket_impl_internal(win_sockets& svc) noexcept;
    ~win_socket_impl_internal();

    void release_internal();

    void connect(
        capy::coro,
        capy::executor_ref,
        endpoint,
        std::stop_token,
        system::error_code*);

    void read_some(
        capy::coro,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*);

    void write_some(
        capy::coro,
        capy::executor_ref,
        io_buffer_param,
        std::stop_token,
        system::error_code*,
        std::size_t*);

    SOCKET native_handle() const noexcept { return socket_; }
    bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }
    void cancel() noexcept;
    void close_socket() noexcept;
    void set_socket(SOCKET s) noexcept { socket_ = s; }
};

//------------------------------------------------------------------------------

/** Socket implementation wrapper for IOCP-based I/O.

    This class is the public-facing socket_impl that holds a shared_ptr
    to the internal state. The shared_ptr is hidden from the public interface.

    @note Internal implementation detail. Users interact with socket class.
*/
class win_socket_impl
    : public socket::socket_impl
    , public intrusive_list<win_socket_impl>::node
{
    std::shared_ptr<win_socket_impl_internal> internal_;

public:
    explicit win_socket_impl(std::shared_ptr<win_socket_impl_internal> internal) noexcept
        : internal_(std::move(internal))
    {
    }

    void release() override;

    void connect(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        endpoint ep,
        std::stop_token token,
        system::error_code* ec) override
    {
        internal_->connect(h, d, ep, token, ec);
    }

    void read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buf,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        internal_->read_some(h, d, buf, token, ec, bytes);
    }

    void write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param buf,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        internal_->write_some(h, d, buf, token, ec, bytes);
    }

    system::error_code shutdown(socket::shutdown_type what) noexcept override
    {
        int how;
        switch (what)
        {
        case socket::shutdown_receive: how = SD_RECEIVE; break;
        case socket::shutdown_send:    how = SD_SEND;    break;
        case socket::shutdown_both:    how = SD_BOTH;    break;
        default:
            return make_err(WSAEINVAL);
        }
        if (::shutdown(internal_->native_handle(), how) != 0)
            return make_err(WSAGetLastError());
        return {};
    }

    native_handle_type native_handle() const noexcept override
    {
        return static_cast<native_handle_type>(internal_->native_handle());
    }

    win_socket_impl_internal* get_internal() const noexcept { return internal_.get(); }
};

//------------------------------------------------------------------------------

/** Internal acceptor state for IOCP-based I/O.

    This class contains the actual state for a listening socket, including
    the native socket handle and pending accept operation.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_acceptor_impl_internal
    : public intrusive_list<win_acceptor_impl_internal>::node
    , public std::enable_shared_from_this<win_acceptor_impl_internal>
{
    friend class win_sockets;
    friend class win_acceptor_impl;

public:
    explicit win_acceptor_impl_internal(win_sockets& svc) noexcept;
    ~win_acceptor_impl_internal();

    void release_internal();

    void accept(
        capy::coro,
        capy::executor_ref,
        std::stop_token,
        system::error_code*,
        io_object::io_object_impl**);

    SOCKET native_handle() const noexcept { return socket_; }
    bool is_open() const noexcept { return socket_ != INVALID_SOCKET; }
    void cancel() noexcept;
    void close_socket() noexcept;

    accept_op acc_;

private:
    win_sockets& svc_;
    SOCKET socket_ = INVALID_SOCKET;
};

//------------------------------------------------------------------------------

/** Acceptor implementation wrapper for IOCP-based I/O.

    This class is the public-facing acceptor_impl that holds a shared_ptr
    to the internal state. The shared_ptr is hidden from the public interface.

    @note Internal implementation detail. Users interact with acceptor class.
*/
class win_acceptor_impl
    : public acceptor::acceptor_impl
    , public intrusive_list<win_acceptor_impl>::node
{
    std::shared_ptr<win_acceptor_impl_internal> internal_;

public:
    explicit win_acceptor_impl(std::shared_ptr<win_acceptor_impl_internal> internal) noexcept
        : internal_(std::move(internal))
    {
    }

    void release() override;

    void accept(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        std::stop_token token,
        system::error_code* ec,
        io_object::io_object_impl** impl_out) override
    {
        internal_->accept(h, d, token, ec, impl_out);
    }

    win_acceptor_impl_internal* get_internal() const noexcept { return internal_.get(); }
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
class win_sockets
    : private win_wsa_init
    , public capy::execution_context::service
{
public:
    using key_type = win_sockets;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_sockets(capy::execution_context& ctx);

    /** Destroy the socket service. */
    ~win_sockets();

    win_sockets(win_sockets const&) = delete;
    win_sockets& operator=(win_sockets const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new socket implementation wrapper.
        The service owns the returned object.
    */
    win_socket_impl& create_impl();

    /** Destroy a socket implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_impl(win_socket_impl& impl);

    /** Unregister a socket implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_impl(win_socket_impl_internal& impl);

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation internal to initialize.
        @return Error code, or success.
    */
    system::error_code open_socket(win_socket_impl_internal& impl);

    /** Create a new acceptor implementation wrapper.
        The service owns the returned object.
    */
    win_acceptor_impl& create_acceptor_impl();

    /** Destroy an acceptor implementation wrapper.
        Removes from tracking list and deletes.
    */
    void destroy_acceptor_impl(win_acceptor_impl& impl);

    /** Unregister an acceptor implementation from the service list.
        Called by the internal impl destructor.
    */
    void unregister_acceptor_impl(win_acceptor_impl_internal& impl);

    /** Create, bind, and listen on an acceptor socket.

        @param impl The acceptor implementation internal to initialize.
        @param ep The local endpoint to bind to.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    system::error_code open_acceptor(
        win_acceptor_impl_internal& impl,
        endpoint ep,
        int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept { return iocp_; }

    /** Return the completion key for associating sockets with IOCP. */
    completion_key* io_key() noexcept { return &overlapped_key_; }

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
    struct overlapped_key final : completion_key
    {
        result on_completion(
            win_scheduler& sched,
            DWORD bytes,
            DWORD dwError,
            LPOVERLAPPED overlapped) override;

        void destroy(LPOVERLAPPED overlapped) override;
    };

    void load_extension_functions();

    win_scheduler& sched_;
    overlapped_key overlapped_key_;
    win_mutex mutex_;
    intrusive_list<win_socket_impl_internal> socket_list_;
    intrusive_list<win_acceptor_impl_internal> acceptor_list_;
    intrusive_list<win_socket_impl> socket_wrapper_list_;
    intrusive_list<win_acceptor_impl> acceptor_wrapper_list_;
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_SOCKETS_HPP
