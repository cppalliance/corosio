//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/detail/iocp/win_tcp_acceptor.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_service.hpp>

#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <Ws2tcpip.h>

namespace boost::corosio::detail {

/** IOCP acceptor service wrapping win_tcp_service for acceptor lifecycle.

    Provides io_service + acceptor_service interface for tcp_acceptor
    on Windows. Delegates to win_tcp_service for actual socket operations.
*/
class BOOST_COROSIO_DECL win_tcp_acceptor_service final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = win_tcp_acceptor_service;

    win_tcp_acceptor_service(capy::execution_context& ctx, win_tcp_service& svc);

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    /** Create the acceptor socket without binding or listening. */
    std::error_code open_acceptor_socket(
        tcp_acceptor::implementation& impl, int family, int type, int protocol);

    /** Bind an open acceptor to a local endpoint. */
    std::error_code
    bind_acceptor(tcp_acceptor::implementation& impl, endpoint ep);

    /** Start listening for incoming connections. */
    std::error_code
    listen_acceptor(tcp_acceptor::implementation& impl, int backlog);

    void shutdown() override;

private:
    win_tcp_service& svc_;
};

// Operation constructors

inline connect_op::connect_op(win_tcp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline read_op::read_op(win_tcp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline write_op::write_op(win_tcp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline wait_op::wait_op(win_tcp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline accept_op::accept_op() noexcept : overlapped_op(&do_complete)
{
    cancel_func_ = &do_cancel_impl;
}

inline acceptor_wait_op::acceptor_wait_op() noexcept
    : overlapped_op(&do_complete)
{
    cancel_func_ = &do_cancel_impl;
}

// Cancellation functions

inline void
connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<connect_op*>(base);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
read_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<read_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
write_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<write_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
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
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
    // wait_type::error parks the op in the auxiliary select reactor;
    // wake it so the reactor can post a cancelled completion. No-op
    // if the reactor was never constructed (e.g. zero-byte WSARecv
    // path was the only thing this socket ever did).
    op->internal.svc_.scheduler().cancel_wait_if_constructed(op);
}

inline void
accept_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<accept_op*>(base);
    if (op->listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->listen_socket), op);
    }
}

inline void
acceptor_wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<acceptor_wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->listen_socket), op);
    }
    if (op->acceptor_ptr)
    {
        op->acceptor_ptr->socket_service().scheduler()
            .cancel_wait_if_constructed(op);
    }
}

// accept_op completion handler

inline void
accept_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<accept_op*>(base);

    if (!owner)
    {
        if (op->accepted_socket != INVALID_SOCKET)
        {
            ::closesocket(op->accepted_socket);
            op->accepted_socket = INVALID_SOCKET;
        }

        if (op->peer_wrapper)
        {
            op->peer_wrapper->close_internal();
            op->peer_wrapper = nullptr;
        }

        op->cleanup_only();
        op->acceptor_ptr.reset();
        return;
    }

    op->stop_cb.reset();

    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));

    if (op->ec_out)
    {
        if (op->cancelled.load(std::memory_order_acquire))
            *op->ec_out = capy::error::canceled;
        else if (op->dwError != 0)
            *op->ec_out = make_err(op->dwError);
        else
            *op->ec_out = {};
    }

    if (success && op->accepted_socket != INVALID_SOCKET && op->peer_wrapper)
    {
        ::setsockopt(
            op->accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&op->listen_socket), sizeof(SOCKET));

        op->peer_wrapper->get_internal()->set_socket(op->accepted_socket);

        sockaddr_storage local_storage{};
        int local_len = sizeof(local_storage);
        sockaddr_storage remote_storage{};
        int remote_len = sizeof(remote_storage);

        endpoint local_ep, remote_ep;
        if (::getsockname(
                op->accepted_socket,
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            local_ep = from_sockaddr(local_storage);
        if (::getpeername(
                op->accepted_socket,
                reinterpret_cast<sockaddr*>(&remote_storage), &remote_len) == 0)
            remote_ep = from_sockaddr(remote_storage);

        op->peer_wrapper->get_internal()->set_endpoints(local_ep, remote_ep);
        op->accepted_socket = INVALID_SOCKET;

        if (op->impl_out)
            *op->impl_out = op->peer_wrapper;
    }
    else
    {
        if (op->accepted_socket != INVALID_SOCKET)
        {
            ::closesocket(op->accepted_socket);
            op->accepted_socket = INVALID_SOCKET;
        }

        if (op->peer_wrapper)
        {
            op->acceptor_ptr->socket_service().destroy(op->peer_wrapper);
            op->peer_wrapper = nullptr;
        }

        if (op->impl_out)
            *op->impl_out = nullptr;
    }

    op->cont_op.cont.h                         = op->h;
    auto saved_ex                      = op->ex;
    auto prevent_premature_destruction = std::move(op->acceptor_ptr);

    dispatch_coro(saved_ex, op->cont_op.cont).resume();
}

// acceptor_wait_op completion handler

inline void
acceptor_wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<acceptor_wait_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->acceptor_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->acceptor_ptr);
    op->invoke_handler();
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
        op->internal_ptr.reset();
        return;
    }

    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success && op->internal.is_open())
    {
        // Required after ConnectEx to enable shutdown(), getsockname(), etc.
        ::setsockopt(
            op->internal.native_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
            nullptr, 0);

        endpoint local_ep;
        sockaddr_storage local_storage{};
        int local_len = sizeof(local_storage);
        if (::getsockname(
                op->internal.native_handle(),
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            local_ep = from_sockaddr(local_storage);
        op->internal.set_endpoints(local_ep, op->target_endpoint);
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
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
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
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
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
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
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// win_tcp_socket_internal

inline win_tcp_socket_internal::win_tcp_socket_internal(win_tcp_service& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
    , wt_(*this)
{
}

inline win_tcp_socket_internal::~win_tcp_socket_internal()
{
    svc_.unregister_impl(*this);
}

inline SOCKET
win_tcp_socket_internal::native_handle() const noexcept
{
    return socket_;
}

inline endpoint
win_tcp_socket_internal::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline endpoint
win_tcp_socket_internal::remote_endpoint() const noexcept
{
    return remote_endpoint_;
}

inline bool
win_tcp_socket_internal::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline void
win_tcp_socket_internal::set_socket(SOCKET s) noexcept
{
    socket_ = s;
}

inline void
win_tcp_socket_internal::set_endpoints(endpoint local, endpoint remote) noexcept
{
    local_endpoint_  = local;
    remote_endpoint_ = remote;
}

inline std::coroutine_handle<>
win_tcp_socket_internal::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    // Keep internal alive during I/O
    conn_.internal_ptr = shared_from_this();

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
win_tcp_socket_internal::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    rd_.internal_ptr = shared_from_this();

    auto& op = rd_;
    op.reset();
    op.is_read  = true;
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

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
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
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
win_tcp_socket_internal::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    wr_.internal_ptr = shared_from_this();

    auto& op = wr_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

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
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
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
win_tcp_socket_internal::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.internal_ptr = shared_from_this();

    auto& op = wt_;
    op.reset();
    op.h            = h;
    op.ex           = d;
    op.ec_out       = ec;
    op.bytes_out    = nullptr;
    op.empty_buffer = true; // skip EOF translation in invoke_handler
    op.start(token);

    svc_.work_started();

    if (w == wait_type::write)
    {
        // Match asio's IOCP behavior and corosio's reactor contract:
        // wait_type::write completes immediately on a connected socket.
        svc_.on_completion(&op, 0, 0);
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

        int result = ::WSARecv(
            socket_, &op.wsabuf, 1, nullptr, &op.flags, &op, nullptr);

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

    // wait_type::error: route through the auxiliary select reactor.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_tcp_socket_internal::cancel() noexcept
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
    // outstanding). Route through the reactor explicitly. Safe no-op
    // if the reactor was never constructed.
    svc_.scheduler().cancel_wait_if_constructed(&wt_);
}

inline void
win_tcp_socket_internal::close_socket() noexcept
{
    // Tear down any aux-reactor-parked wait op before closing the
    // SOCKET handle. Otherwise the reactor would keep polling a
    // dangling fd (and on a Winsock SOCKET-id reuse the wrong fd
    // could be polled briefly). The cancel happens-before the
    // closesocket below.
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);

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

// win_tcp_socket

inline win_tcp_socket::win_tcp_socket(
    std::shared_ptr<win_tcp_socket_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_tcp_socket::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_socket();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_tcp_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->connect(h, d, ep, token, ec);
}

inline std::coroutine_handle<>
win_tcp_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->read_some(h, d, buf, token, ec, bytes);
}

inline std::coroutine_handle<>
win_tcp_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->write_some(h, d, buf, token, ec, bytes);
}

inline std::coroutine_handle<>
win_tcp_socket::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->wait(h, d, w, token, ec);
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
    if (::shutdown(internal_->native_handle(), how) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline native_handle_type
win_tcp_socket::native_handle() const noexcept
{
    return static_cast<native_handle_type>(internal_->native_handle());
}

inline std::error_code
win_tcp_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char const*>(data), static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_tcp_socket::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    int len = static_cast<int>(*size);
    if (::getsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline endpoint
win_tcp_socket::local_endpoint() const noexcept
{
    return internal_->local_endpoint();
}

inline endpoint
win_tcp_socket::remote_endpoint() const noexcept
{
    return internal_->remote_endpoint();
}

inline void
win_tcp_socket::cancel() noexcept
{
    internal_->cancel();
}

inline win_tcp_socket_internal*
win_tcp_socket::get_internal() const noexcept
{
    return internal_.get();
}

// win_tcp_service

inline win_tcp_service::win_tcp_service(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
    load_extension_functions();
}

inline win_tcp_service::~win_tcp_service()
{
    // Delete wrappers that survived shutdown. This runs after
    // win_scheduler is destroyed (reverse creation order), so
    // all coroutine frames and their tcp_socket members are gone.
    for (auto* w = socket_wrapper_list_.pop_front(); w != nullptr;
         w       = socket_wrapper_list_.pop_front())
        delete w;

    for (auto* w = acceptor_wrapper_list_.pop_front(); w != nullptr;
         w       = acceptor_wrapper_list_.pop_front())
        delete w;
}

inline void
win_tcp_service::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    // Close all sockets to force pending I/O to complete via IOCP.
    // Wrappers are NOT deleted here - coroutine frames destroyed
    // during scheduler shutdown may still hold tcp_socket objects
    // that reference them. Wrapper deletion is deferred to ~win_tcp_service
    // after the scheduler has drained all outstanding operations.
    for (auto* impl = socket_list_.pop_front(); impl != nullptr;
         impl       = socket_list_.pop_front())
    {
        impl->close_socket();
    }

    for (auto* impl = acceptor_list_.pop_front(); impl != nullptr;
         impl       = acceptor_list_.pop_front())
    {
        impl->close_socket();
    }
}

inline io_object::implementation*
win_tcp_service::construct()
{
    auto internal = std::make_shared<win_tcp_socket_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.push_back(internal.get());
    }

    auto* wrapper = new win_tcp_socket(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_tcp_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_tcp_socket&>(*p);
        wrapper.close_internal();
        destroy_impl(wrapper);
    }
}

inline void
win_tcp_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_tcp_socket&>(*h.get());
    wrapper.get_internal()->close_socket();
}

inline void
win_tcp_service::destroy_impl(win_tcp_socket& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_tcp_service::unregister_impl(win_tcp_socket_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    socket_list_.remove(&impl);
}

inline std::error_code
win_tcp_service::open_socket(
    win_tcp_socket_internal& impl, int family, int type, int protocol)
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
win_tcp_service::bind_socket(win_tcp_socket_internal& impl, endpoint ep)
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
win_tcp_service::on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept
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

inline void
win_tcp_service::destroy_acceptor_impl(win_tcp_acceptor& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_tcp_service::unregister_acceptor_impl(win_tcp_acceptor_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    acceptor_list_.remove(&impl);
}

inline std::error_code
win_tcp_service::open_acceptor_socket(
    win_tcp_acceptor_internal& impl, int family, int type, int protocol)
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
    return {};
}

inline std::error_code
win_tcp_service::bind_acceptor(win_tcp_acceptor_internal& impl, endpoint ep)
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
win_tcp_service::listen_acceptor(win_tcp_acceptor_internal& impl, int backlog)
{
    SOCKET sock = impl.socket_;

    if (::listen(sock, backlog) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    return {};
}

// win_tcp_acceptor_internal

inline win_tcp_acceptor_internal::win_tcp_acceptor_internal(win_tcp_service& svc) noexcept
    : svc_(svc)
{
}

inline win_tcp_acceptor_internal::~win_tcp_acceptor_internal()
{
    svc_.unregister_acceptor_impl(*this);
}

inline win_tcp_service&
win_tcp_acceptor_internal::socket_service() noexcept
{
    return svc_;
}

inline SOCKET
win_tcp_acceptor_internal::native_handle() const noexcept
{
    return socket_;
}

inline endpoint
win_tcp_acceptor_internal::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline bool
win_tcp_acceptor_internal::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline void
win_tcp_acceptor_internal::set_local_endpoint(endpoint ep) noexcept
{
    local_endpoint_ = ep;
}

inline std::coroutine_handle<>
win_tcp_acceptor_internal::accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    // Keep acceptor internal alive during I/O
    acc_.acceptor_ptr = shared_from_this();

    auto& op = acc_;
    op.reset();
    op.h        = h;
    op.ex       = d;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.start(token);

    svc_.work_started();

    // Create wrapper for the peer socket (service owns it)
    auto& peer_wrapper = static_cast<win_tcp_socket&>(*svc_.construct());

    // Derive AF from the listening socket's cached local endpoint
    int af = local_endpoint_.is_v6() ? AF_INET6 : AF_INET;

    // Create the accepted socket with matching address family
    SOCKET accepted = ::WSASocketW(
        af, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (accepted == INVALID_SOCKET)
    {
        svc_.destroy(&peer_wrapper);
        svc_.on_completion(&op, ::WSAGetLastError(), 0);
        return std::noop_coroutine();
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(accepted), svc_.native_handle(), key_io, 0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(accepted);
        svc_.destroy(&peer_wrapper);
        svc_.on_completion(&op, err, 0);
        return std::noop_coroutine();
    }

    // Set up the accept operation
    op.accepted_socket = accepted;
    op.peer_wrapper    = &peer_wrapper;
    op.listen_socket   = socket_;

    auto accept_ex = svc_.accept_ex();
    if (!accept_ex)
    {
        ::closesocket(accepted);
        svc_.destroy(&peer_wrapper);
        op.peer_wrapper    = nullptr;
        op.accepted_socket = INVALID_SOCKET;
        svc_.on_completion(&op, WSAEOPNOTSUPP, 0);
        return std::noop_coroutine();
    }

    // AcceptEx address buffer sizes must match the socket's address family
    DWORD addr_size = static_cast<DWORD>(
        (af == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in)) + 16);
    DWORD bytes_received = 0;

    BOOL ok = accept_ex(
        socket_, accepted, op.addr_buf, 0, addr_size, addr_size,
        &bytes_received, &op);

    if (!ok)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            ::closesocket(accepted);
            svc_.destroy(&peer_wrapper);
            op.peer_wrapper    = nullptr;
            op.accepted_socket = INVALID_SOCKET;
            svc_.on_completion(&op, err, 0);
            return std::noop_coroutine();
        }
    }

    svc_.on_pending(&op);

    // If the stop_token was already cancelled when start() was called,
    // the CancelIoEx in the canceller fired before AcceptEx was
    // submitted and had no effect.  Re-issue now that the I/O is
    // pending so the completion posts with ERROR_OPERATION_ABORTED.
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_acceptor_internal::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.acceptor_ptr  = shared_from_this();
    wt_.listen_socket = socket_;

    auto& op = wt_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = nullptr;
    op.start(token);

    svc_.work_started();

    if (w == wait_type::write)
    {
        svc_.on_completion(&op, 0, 0);
        return std::noop_coroutine();
    }

    // wait_type::read (incoming connection ready) and wait_type::error
    // on the listen socket route through the auxiliary select reactor.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_tcp_acceptor_internal::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }

    acc_.request_cancel();
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);
}

inline void
win_tcp_acceptor_internal::close_socket() noexcept
{
    // Tear down any aux-reactor-parked wait op first.
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    // Clear cached endpoint
    local_endpoint_ = endpoint{};
}

// win_tcp_acceptor

inline win_tcp_acceptor::win_tcp_acceptor(
    std::shared_ptr<win_tcp_acceptor_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_tcp_acceptor::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_socket();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_tcp_acceptor::accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    return internal_->accept(h, d, token, ec, impl_out);
}

inline std::coroutine_handle<>
win_tcp_acceptor::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->wait(h, d, w, token, ec);
}

inline endpoint
win_tcp_acceptor::local_endpoint() const noexcept
{
    return internal_->local_endpoint();
}

inline bool
win_tcp_acceptor::is_open() const noexcept
{
    return internal_ && internal_->is_open();
}

inline void
win_tcp_acceptor::cancel() noexcept
{
    internal_->cancel();
}

inline std::error_code
win_tcp_acceptor::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char const*>(data), static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_tcp_acceptor::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    int len = static_cast<int>(*size);
    if (::getsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline win_tcp_acceptor_internal*
win_tcp_acceptor::get_internal() const noexcept
{
    return internal_.get();
}

// win_tcp_acceptor_service

inline win_tcp_acceptor_service::win_tcp_acceptor_service(
    capy::execution_context& ctx, win_tcp_service& svc)
    : svc_(svc)
{
    (void)ctx;
}

inline io_object::implementation*
win_tcp_acceptor_service::construct()
{
    auto internal = std::make_shared<win_tcp_acceptor_internal>(svc_);

    {
        std::lock_guard<win_mutex> lock(svc_.mutex_);
        svc_.acceptor_list_.push_back(internal.get());
    }

    auto* wrapper = new win_tcp_acceptor(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(svc_.mutex_);
        svc_.acceptor_wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_tcp_acceptor_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_tcp_acceptor&>(*p);
        wrapper.close_internal();
        svc_.destroy_acceptor_impl(wrapper);
    }
}

inline void
win_tcp_acceptor_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_tcp_acceptor&>(*h.get());
    wrapper.get_internal()->close_socket();
}

inline std::error_code
win_tcp_acceptor_service::open_acceptor_socket(
    tcp_acceptor::implementation& impl, int family, int type, int protocol)
{
    auto& wrapper = static_cast<win_tcp_acceptor&>(impl);
    return svc_.open_acceptor_socket(
        *wrapper.get_internal(), family, type, protocol);
}

inline std::error_code
win_tcp_acceptor_service::bind_acceptor(
    tcp_acceptor::implementation& impl, endpoint ep)
{
    auto& wrapper = static_cast<win_tcp_acceptor&>(impl);
    return svc_.bind_acceptor(*wrapper.get_internal(), ep);
}

inline std::error_code
win_tcp_acceptor_service::listen_acceptor(
    tcp_acceptor::implementation& impl, int backlog)
{
    auto& wrapper = static_cast<win_tcp_acceptor&>(impl);
    return svc_.listen_acceptor(*wrapper.get_internal(), backlog);
}

inline void
win_tcp_acceptor_service::shutdown()
{
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_ACCEPTOR_SERVICE_HPP
