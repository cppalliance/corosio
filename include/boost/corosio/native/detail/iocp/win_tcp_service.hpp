//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_dissociate.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>

#include <boost/corosio/native/detail/iocp/win_tcp_socket.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor.hpp>

#include <mswsock.h>

namespace boost::corosio::detail {

class win_scheduler;
class win_tcp_acceptor;
class win_tcp_acceptor_service;

/** Windows IOCP socket management service.

    This service owns all socket and acceptor implementations and
    coordinates their lifecycle with the IOCP. It provides:

    - Socket/acceptor implementation recycling through `object_pool`
    - IOCP handle association for sockets
    - Function pointer loading for ConnectEx/AcceptEx
    - Graceful shutdown - closes every live implementation when the
      io_context stops; pool destruction frees whatever is left

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class BOOST_COROSIO_DECL win_tcp_service final
    : private win_wsa_init
    , public capy::execution_context::service
    , public io_object::io_service
{
    friend class win_tcp_socket;
    friend class win_tcp_acceptor;

public:
    using key_type = win_tcp_service;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_tcp_service(capy::execution_context& ctx);

    win_tcp_service(win_tcp_service const&)            = delete;
    win_tcp_service& operator=(win_tcp_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation to initialize.
        @return Error code, or success.
    */
    std::error_code open_socket(
        win_tcp_socket& impl, int family, int type, int protocol);

    /** Adopt an existing socket handle into an implementation.

        Validates family and type before touching the held socket,
        then associates the new socket with the IOCP. On success the
        impl takes ownership and will close the handle; on failure
        the caller retains ownership.

        @param impl The socket implementation to assign to.
        @param fd The native socket handle to adopt.
        @return Error code, or success.
    */
    std::error_code assign_socket(win_tcp_socket& impl, native_handle_type fd);

    /** Bind a stream socket to a local endpoint.

        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code bind_socket(win_tcp_socket& impl, endpoint ep);

    /** Pop a recycled acceptor or news one, with the service reference
        held. Used by `win_tcp_acceptor_service::construct()`.
    */
    win_tcp_acceptor* acquire_acceptor_impl();

    /** Create an acceptor socket without binding or listening.

        Creates a socket and associates it with the IOCP.
        For IPv6, dual-stack is enabled by default.
        Does not set SO_REUSEADDR.

        @param impl The acceptor implementation to initialize.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code, or success.
    */
    std::error_code open_acceptor_socket(
        win_tcp_acceptor& impl, int family, int type, int protocol);

    /** Adopt an existing listening socket into an acceptor.

        Validates the socket, associates it with the IOCP, and only
        then releases the socket the acceptor already held. Listen
        state is not verified.

        @param impl The acceptor implementation.
        @param fd The native socket to adopt. Ownership transfers only
            on success.
        @return Error code, or success.
    */
    std::error_code
    assign_acceptor_socket(win_tcp_acceptor& impl, native_handle_type fd);

    /** Bind an open acceptor to a local endpoint.

        @param impl The acceptor implementation.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code bind_acceptor(win_tcp_acceptor& impl, endpoint ep);

    /** Start listening for incoming connections.

        @param impl The acceptor implementation.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    std::error_code listen_acceptor(win_tcp_acceptor& impl, int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept;

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept
    {
        return connect_ex_;
    }

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept
    {
        return accept_ex_;
    }

    /** Post an overlapped operation for completion. */
    void post(overlapped_op* op);

    /** Signal that an overlapped I/O is now pending (CAS protocol). */
    void on_pending(overlapped_op* op) noexcept;

    /** Post an immediate completion with pre-stored results. */
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept;

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

    /** Return the owning IOCP scheduler. */
    win_scheduler& scheduler() noexcept
    {
        return sched_;
    }

private:
    void load_extension_functions();

    win_scheduler& sched_;
    void* iocp_;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_   = nullptr;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_tcp_socket> pool_;
    object_pool<win_tcp_acceptor> acceptor_pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

// Operation constructors

inline connect_op::connect_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline read_op::read_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline write_op::write_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline wait_op::wait_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

// Cancellation functions

inline void
connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<connect_op*>(base);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
read_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<read_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
write_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<write_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    // Best-effort cancel of any pending zero-byte WSARecv issued for
    // wait_type::read. ERROR_NOT_FOUND when nothing overlapped is
    // pending is harmless.
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
    // wait_type::error parks the op in the auxiliary select reactor;
    // wake it so the reactor can post a cancelled completion. A cancel
    // for an op the reactor never registered finds nothing and returns.
    op->internal.svc_.scheduler().cancel_wait(op);
}

// connect_op completion handler

inline void
connect_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<connect_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->object_ref_.reset();
        return;
    }

    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success && op->internal.socket_ != INVALID_SOCKET)
    {
        // Required after ConnectEx to enable shutdown(), getsockname(), etc.
        ::setsockopt(
            op->internal.socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
            nullptr, 0);

        endpoint local_ep;
        sockaddr_storage local_storage{};
        int local_len = sizeof(local_storage);
        if (::getsockname(
                op->internal.socket_,
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            local_ep = from_sockaddr(local_storage);
        op->internal.set_endpoints(local_ep, op->target_endpoint);
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// read_op completion handler

inline void
read_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<read_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->object_ref_.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// write_op completion handler

inline void
write_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<write_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->object_ref_.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// wait_op completion handler

inline void
wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<wait_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->object_ref_.reset();
        return;
    }

    // Only the zero-byte WSARecv read wait; write and error waits come
    // from the poll reactor through this op too.
    if (op->w == wait_type::read)
        op->dwError = normalize_read_wait_error(op->dwError);

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// win_tcp_socket

inline win_tcp_socket::win_tcp_socket(win_tcp_service& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
    , wt_(*this)
{
}

inline void
win_tcp_socket::retire() noexcept
{
    svc_.pool_.recycle(this);
}

inline void
win_tcp_socket::reuse() noexcept
{
    BOOST_COROSIO_ASSERT(socket_ == INVALID_SOCKET);
    BOOST_COROSIO_ASSERT(family_ == AF_UNSPEC);
    // A connect completing on another thread while the socket closed
    // can still have written these.
    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
    BOOST_COROSIO_ASSERT(!conn_.stop_cb);
    BOOST_COROSIO_ASSERT(!rd_.stop_cb);
    BOOST_COROSIO_ASSERT(!wr_.stop_cb);
    BOOST_COROSIO_ASSERT(!wt_.stop_cb);
}

inline void
win_tcp_socket::set_socket(SOCKET s) noexcept
{
    socket_ = s;
}

inline void
win_tcp_socket::set_endpoints(endpoint local, endpoint remote) noexcept
{
    local_endpoint_  = local;
    remote_endpoint_ = remote;
}

inline std::coroutine_handle<>
win_tcp_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    // Keep this socket alive during I/O
    conn_.object_ref_ = detail::object_ref(this);

    auto& op = conn_;
    op.reset();
    op.h               = h;
    op.ex              = d;
    op.ec_out          = ec;
    op.target_endpoint = ep;
    op.start(token);

    svc_.work_started();

    // ConnectEx requires the socket to be bound. Skip if already bound
    // (e.g. the caller used tcp_socket::bind() before connect).
    if (local_endpoint_ == endpoint{})
    {
        sockaddr_storage bind_storage{};
        socklen_t bind_len;
        if (family_ == AF_INET6)
        {
            sockaddr_in6 sa6{};
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port   = 0;
            sa6.sin6_addr   = in6addr_any;
            std::memcpy(&bind_storage, &sa6, sizeof(sa6));
            bind_len = sizeof(sa6);
        }
        else
        {
            sockaddr_in sa4{};
            sa4.sin_family      = AF_INET;
            sa4.sin_addr.s_addr = INADDR_ANY;
            sa4.sin_port        = 0;
            std::memcpy(&bind_storage, &sa4, sizeof(sa4));
            bind_len = sizeof(sa4);
        }

        if (::bind(
                socket_, reinterpret_cast<sockaddr*>(&bind_storage),
                bind_len) == SOCKET_ERROR)
        {
            svc_.on_completion(&op, ::WSAGetLastError(), 0);
            return std::noop_coroutine();
        }
    }

    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        svc_.on_completion(&op, WSAEOPNOTSUPP, 0);
        return std::noop_coroutine();
    }

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, family_, storage);

    BOOL result = connect_ex(
        socket_, reinterpret_cast<sockaddr*>(&storage),
        static_cast<int>(addrlen), nullptr, 0, nullptr, &op);

    if (!result)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            svc_.on_completion(&op, err, 0);
            return std::noop_coroutine();
        }
    }

    svc_.on_pending(&op);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep this socket alive during I/O
    rd_.object_ref_ = detail::object_ref(this);

    auto& op = rd_;
    op.reset();
    op.is_read   = true;
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel.
    if (socket_ == INVALID_SOCKET)
    {
        svc_.on_completion(&op, WSAEBADF, 0);
        return std::noop_coroutine();
    }

    // Prepare buffers
    capy::mutable_buffer bufs[read_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, read_op::max_buffers));

    // Handle empty buffer: complete with 0 bytes
    if (op.wsabuf_count == 0)
    {
        op.empty_buffer = true;
        svc_.on_completion(&op, 0, 0);
        return std::noop_coroutine();
    }

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(
            (std::min)(bufs[i].size(), std::size_t(0x7fffffff)));
    }

    op.flags = 0;

    int result = ::WSARecv(
        socket_, op.wsabufs, op.wsabuf_count, nullptr, &op.flags, &op, nullptr);

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            svc_.on_completion(&op, err, 0);
            return std::noop_coroutine();
        }
    }

    svc_.on_pending(&op);

    // Re-check cancellation after I/O is pending
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep this socket alive during I/O
    wr_.object_ref_ = detail::object_ref(this);

    auto& op = wr_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel.
    if (socket_ == INVALID_SOCKET)
    {
        svc_.on_completion(&op, WSAEBADF, 0);
        return std::noop_coroutine();
    }

    // Prepare buffers
    capy::mutable_buffer bufs[write_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, write_op::max_buffers));

    // Handle empty buffer: complete immediately with 0 bytes
    if (op.wsabuf_count == 0)
    {
        svc_.on_completion(&op, 0, 0);
        return std::noop_coroutine();
    }

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(
            (std::min)(bufs[i].size(), std::size_t(0x7fffffff)));
    }

    int result = ::WSASend(
        socket_, op.wsabufs, op.wsabuf_count, nullptr, 0, &op, nullptr);

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            svc_.on_completion(&op, err, 0);
            return std::noop_coroutine();
        }
    }

    svc_.on_pending(&op);

    // Re-check cancellation after I/O is pending
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.object_ref_ = detail::object_ref(this);

    auto& op = wt_;
    op.reset();
    op.h            = h;
    op.ex           = d;
    op.ec_out       = ec;
    op.bytes_out    = nullptr;
    op.empty_buffer = true; // skip EOF translation in invoke_handler
    op.w            = w;
    op.start(token);

    svc_.work_started();

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel or the wait reactor.
    if (socket_ == INVALID_SOCKET)
    {
        svc_.on_completion(&op, WSAEBADF, 0);
        return std::noop_coroutine();
    }

    if (w == wait_type::read)
    {
        // Zero-byte WSARecv: kernel signals completion when data is
        // available without consuming any bytes from the stream. This
        // is the documented Winsock pattern for "is the socket
        // readable" notifications.
        op.wsabuf = WSABUF{0, nullptr};
        op.flags  = 0;

        int result =
            ::WSARecv(socket_, &op.wsabuf, 1, nullptr, &op.flags, &op, nullptr);

        if (result == SOCKET_ERROR)
        {
            DWORD err = ::WSAGetLastError();
            if (err != WSA_IO_PENDING)
            {
                svc_.on_completion(&op, err, 0);
                return std::noop_coroutine();
            }
        }

        svc_.on_pending(&op);

        // Re-check cancellation after I/O is pending.
        if (op.cancelled.load(std::memory_order_acquire))
            ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

        return std::noop_coroutine();
    }

    // wait_type::write and wait_type::error: route through the
    // auxiliary poll reactor. There is no overlapped primitive for
    // "the send buffer has room" that does not also transfer bytes,
    // and a write wait must report real writability.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_tcp_socket::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    wt_.request_cancel();
    // CancelIoEx covers overlapped I/O on the socket but cannot reach
    // a wait op parked in the auxiliary reactor (no overlapped is
    // outstanding). Route through the reactor explicitly.
    svc_.scheduler().cancel_wait(&wt_);
}

inline void
win_tcp_socket::close_socket() noexcept
{
    // Flag every op cancelled before tearing down the handle. closesocket()
    // can complete a pending overlapped op with ERROR_NETNAME_DELETED (not the
    // MSDN-documented ERROR_OPERATION_ABORTED), which iocp_make_err now maps to
    // connection_reset -- so a locally-closed op must be short-circuited to
    // canceled via the flag rather than relying on the raw error, exactly as
    // cancel() does. The store happens-before the completion is decoded.
    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    // wt_ is parked in the aux reactor (no overlapped outstanding), so it also
    // needs an explicit reactor deregister before the SOCKET handle is closed;
    // otherwise the reactor would keep polling a dangling fd (and on a Winsock
    // SOCKET-id reuse the wrong fd could be polled briefly).
    wt_.request_cancel();
    svc_.scheduler().cancel_wait(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    family_ = AF_UNSPEC;

    // Clear cached endpoints
    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
}

inline std::error_code
win_tcp_socket::shutdown(tcp_socket::shutdown_type what) noexcept
{
    int how;
    switch (what)
    {
    case tcp_socket::shutdown_receive:
        how = SD_RECEIVE;
        break;
    case tcp_socket::shutdown_send:
        how = SD_SEND;
        break;
    case tcp_socket::shutdown_both:
        how = SD_BOTH;
        break;
    default:
        return make_err(WSAEINVAL);
    }
    if (::shutdown(socket_, how) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline native_handle_type
win_tcp_socket::native_handle() const noexcept
{
    return static_cast<native_handle_type>(socket_);
}

inline native_handle_type
win_tcp_socket::release_socket() noexcept
{
    SOCKET s = socket_;
    if (s != INVALID_SOCKET)
    {
        cancel();
        // Sever the port association so the descriptor can be
        // adopted again; best-effort, the caller keeps a working
        // socket either way.
        dissociate_from_iocp(s);
        socket_          = INVALID_SOCKET;
        family_          = AF_UNSPEC;
        local_endpoint_  = endpoint{};
        remote_endpoint_ = endpoint{};
    }
    return static_cast<native_handle_type>(s);
}

inline std::error_code
win_tcp_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            socket_, level, optname, reinterpret_cast<char const*>(data),
            static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_tcp_socket::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    int len = static_cast<int>(*size);
    if (::getsockopt(
            socket_, level, optname, reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline endpoint
win_tcp_socket::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline endpoint
win_tcp_socket::remote_endpoint() const noexcept
{
    return remote_endpoint_;
}

// win_tcp_service

inline win_tcp_service::win_tcp_service(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
    load_extension_functions();
}

inline void
win_tcp_service::shutdown()
{
    // Close all sockets/acceptors to force pending I/O to complete via
    // IOCP. Not deleted here: in-flight ops hold their own object_ref, so
    // each impl stays alive until its cancel completion drains; the
    // pools' own destructors free whatever is still live once this
    // service itself is torn down (after the scheduler, which drains
    // every queued completion first).
    pool_.shutdown([](win_tcp_socket* impl) { impl->close_socket(); });

    acceptor_pool_.shutdown(
        [](win_tcp_acceptor* impl) { impl->close_socket(); });
}

inline io_object::implementation*
win_tcp_service::construct()
{
    return pool_.acquire(*this);
}

inline void
win_tcp_service::destroy(io_object::implementation* p)
{
    if (!p)
        return;
    auto* s = static_cast<win_tcp_socket*>(p);
    s->close_socket();
    release(s);
}

inline void
win_tcp_service::close(io_object::handle& h)
{
    static_cast<win_tcp_socket*>(h.get())->close_socket();
}

inline std::error_code
win_tcp_service::open_socket(
    win_tcp_socket& impl, int family, int type, int protocol)
{
    impl.close_socket();

    SOCKET sock =
        ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return make_err(::WSAGetLastError());

    if (family == AF_INET6)
    {
        DWORD one = 1;
        ::setsockopt(
            sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&one),
            sizeof(one));
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    impl.socket_ = sock;
    impl.family_ = family;
    return {};
}

inline std::error_code
win_tcp_service::assign_socket(win_tcp_socket& impl, native_handle_type fd)
{
    SOCKET sock = static_cast<SOCKET>(fd);
    if (sock == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);

    // SO_PROTOCOL_INFOW works on an unbound socket, unlike getsockname
    // (WSAEINVAL until bind/connect names it) -- an adopted socket may
    // have reached connected state without an explicit bind.
    WSAPROTOCOL_INFOW proto_info{};
    int proto_len = sizeof(proto_info);
    if (::getsockopt(
            sock, SOL_SOCKET, SO_PROTOCOL_INFOW,
            reinterpret_cast<char*>(&proto_info), &proto_len) != 0)
        return make_err(::WSAGetLastError());
    if (proto_info.iAddressFamily != AF_INET &&
        proto_info.iAddressFamily != AF_INET6)
        return make_err(WSAEAFNOSUPPORT);
    if (proto_info.iSocketType != SOCK_STREAM)
        return make_err(WSAEPROTOTYPE);

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);
    if (result == nullptr)
        return make_err(::GetLastError());

    impl.socket_ = sock;
    impl.family_ = proto_info.iAddressFamily;

    endpoint local_ep, remote_ep;
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        local_ep = detail::from_sockaddr(local_storage);
    sockaddr_storage remote_storage{};
    int remote_len = sizeof(remote_storage);
    if (::getpeername(
            sock, reinterpret_cast<sockaddr*>(&remote_storage), &remote_len) ==
        0)
        remote_ep = detail::from_sockaddr(remote_storage);
    impl.set_endpoints(local_ep, remote_ep);

    return {};
}

inline std::error_code
win_tcp_service::bind_socket(win_tcp_socket& impl, endpoint ep)
{
    SOCKET sock = impl.socket_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(
            sock, reinterpret_cast<sockaddr*>(&storage),
            static_cast<int>(addrlen)) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    // Cache local endpoint (resolves ephemeral port)
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        impl.local_endpoint_ = detail::from_sockaddr(local_storage);

    return {};
}

inline void*
win_tcp_service::native_handle() const noexcept
{
    return iocp_;
}

inline void
win_tcp_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_tcp_service::on_pending(overlapped_op* op) noexcept
{
    sched_.on_pending(op);
}

inline void
win_tcp_service::on_completion(
    overlapped_op* op, DWORD error, DWORD bytes) noexcept
{
    sched_.on_completion(op, error, bytes);
}

inline void
win_tcp_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_tcp_service::work_finished() noexcept
{
    sched_.work_finished();
}

inline void
win_tcp_service::load_extension_functions()
{
    SOCKET sock = ::WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return;

    DWORD bytes = 0;

    GUID connect_ex_guid = WSAID_CONNECTEX;
    ::WSAIoctl(
        sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_ex_guid,
        sizeof(connect_ex_guid), &connect_ex_, sizeof(connect_ex_), &bytes,
        nullptr, nullptr);

    GUID accept_ex_guid = WSAID_ACCEPTEX;
    ::WSAIoctl(
        sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_ex_guid,
        sizeof(accept_ex_guid), &accept_ex_, sizeof(accept_ex_), &bytes,
        nullptr, nullptr);

    ::closesocket(sock);
}

inline win_tcp_acceptor*
win_tcp_service::acquire_acceptor_impl()
{
    return acceptor_pool_.acquire(*this);
}

inline std::error_code
win_tcp_service::open_acceptor_socket(
    win_tcp_acceptor& impl, int family, int type, int protocol)
{
    impl.close_socket();

    SOCKET sock =
        ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return make_err(::WSAGetLastError());

    if (family == AF_INET6)
    {
        DWORD val = 0; // dual-stack default
        ::setsockopt(
            sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&val),
            sizeof(val));
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    impl.socket_ = sock;
    impl.family_ = family;
    return {};
}

inline std::error_code
win_tcp_service::assign_acceptor_socket(
    win_tcp_acceptor& impl, native_handle_type fd)
{
    SOCKET sock = static_cast<SOCKET>(fd);
    if (sock == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);

    // SO_PROTOCOL_INFOW works on an unbound socket, unlike getsockname
    // (WSAEINVAL until bind names it).
    WSAPROTOCOL_INFOW proto_info{};
    int proto_len = sizeof(proto_info);
    if (::getsockopt(
            sock, SOL_SOCKET, SO_PROTOCOL_INFOW,
            reinterpret_cast<char*>(&proto_info), &proto_len) != 0)
        return make_err(::WSAGetLastError());
    if (proto_info.iAddressFamily != AF_INET &&
        proto_info.iAddressFamily != AF_INET6)
        return make_err(WSAEAFNOSUPPORT);
    if (proto_info.iSocketType != SOCK_STREAM)
        return make_err(WSAEPROTOTYPE);

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);
    if (result == nullptr)
        return make_err(::GetLastError());

    impl.socket_ = sock;
    impl.family_ = proto_info.iAddressFamily;

    // AcceptEx sizes its address buffers from this cache, so an
    // unseeded endpoint breaks accepts on an adopted v6 listener.
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        impl.set_local_endpoint(detail::from_sockaddr(local_storage));

    return {};
}

inline std::error_code
win_tcp_service::bind_acceptor(win_tcp_acceptor& impl, endpoint ep)
{
    SOCKET sock = impl.socket_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(
            sock, reinterpret_cast<sockaddr*>(&storage),
            static_cast<int>(addrlen)) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    // Cache local endpoint (resolves ephemeral port)
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        impl.set_local_endpoint(detail::from_sockaddr(local_storage));

    return {};
}

inline std::error_code
win_tcp_service::listen_acceptor(win_tcp_acceptor& impl, int backlog)
{
    SOCKET sock = impl.socket_;

    if (::listen(sock, backlog) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
