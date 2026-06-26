//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/udp_service.hpp>

#include <boost/corosio/native/detail/iocp/win_udp_socket.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <cstring>

#include <Ws2tcpip.h>

namespace boost::corosio::detail {

/** IOCP UDP service implementation.

    Inherits from udp_service to enable runtime polymorphism.
    Uses key_type = udp_service for service lookup.
*/
class BOOST_COROSIO_DECL win_udp_service final
    : private win_wsa_init
    , public udp_service
{
public:
    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    explicit win_udp_service(capy::execution_context& ctx);

    ~win_udp_service();

    win_udp_service(win_udp_service const&)            = delete;
    win_udp_service& operator=(win_udp_service const&) = delete;

    void shutdown() override;

    std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;
    std::error_code
    bind_datagram(udp_socket::implementation& impl, endpoint ep) override;

    void destroy_impl(win_udp_socket& impl);

    void unregister_impl(win_udp_socket_internal& impl);

    std::error_code open_socket(
        win_udp_socket_internal& impl, int family, int type, int protocol);

    void post(overlapped_op* op);
    void on_pending(overlapped_op* op) noexcept;
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept;
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Return the owning IOCP scheduler. */
    win_scheduler& scheduler() noexcept
    {
        return sched_;
    }

private:
    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    win_mutex mutex_;
    intrusive_list<win_udp_socket_internal> socket_list_;
    intrusive_list<win_udp_socket> wrapper_list_;
    BOOST_COROSIO_MSVC_WARNING_POP
    void* iocp_;
};

// Operation constructors

inline send_to_op::send_to_op(win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline recv_from_op::recv_from_op(win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline udp_connect_op::udp_connect_op(
    win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline udp_send_op::udp_send_op(win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline udp_recv_op::udp_recv_op(win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline udp_wait_op::udp_wait_op(win_udp_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

// Cancellation functions

inline void
send_to_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<send_to_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
recv_from_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<recv_from_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

// send_to_op completion handler

inline void
send_to_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<send_to_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// recv_from_op completion handler

inline void
recv_from_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<recv_from_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    // Extract source endpoint on success
    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success && op->source_out)
    {
        *op->source_out = from_sockaddr(op->source_storage);
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// Connected-mode cancellation

inline void
udp_connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<udp_connect_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
}

inline void
udp_send_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<udp_send_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
udp_recv_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<udp_recv_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
udp_wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<udp_wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
    op->internal.svc_.scheduler().cancel_wait_if_constructed(op);
}

// Connected-mode completion handlers

inline void
udp_connect_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<udp_connect_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    // Cache endpoints on success
    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success)
    {
        sockaddr_storage local_storage{};
        int local_len = sizeof(local_storage);
        if (::getsockname(
                op->internal.socket_,
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            op->internal.local_endpoint_ = from_sockaddr(local_storage);
        op->internal.remote_endpoint_ = op->target_endpoint;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

inline void
udp_send_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<udp_send_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

inline void
udp_recv_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<udp_recv_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

inline void
udp_wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<udp_wait_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// win_udp_socket_internal

inline win_udp_socket_internal::win_udp_socket_internal(
    win_udp_service& svc) noexcept
    : svc_(svc)
    , wr_(*this)
    , rd_(*this)
    , conn_(*this)
    , send_wr_(*this)
    , recv_rd_(*this)
    , wt_(*this)
{
}

inline win_udp_socket_internal::~win_udp_socket_internal()
{
    svc_.unregister_impl(*this);
}

inline SOCKET
win_udp_socket_internal::native_handle() const noexcept
{
    return socket_;
}

inline endpoint
win_udp_socket_internal::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline endpoint
win_udp_socket_internal::remote_endpoint() const noexcept
{
    return remote_endpoint_;
}

inline bool
win_udp_socket_internal::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline std::coroutine_handle<>
win_udp_socket_internal::send_to(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    endpoint dest,
    int flags,
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
    capy::mutable_buffer bufs[send_to_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, send_to_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    // Prepare destination address
    op.dest_len = static_cast<int>(to_sockaddr(dest, family_, op.dest_storage));

    int result = ::WSASendTo(
        socket_, op.wsabufs, op.wsabuf_count, nullptr,
        static_cast<DWORD>(flags),
        reinterpret_cast<sockaddr*>(&op.dest_storage), op.dest_len, &op,
        nullptr);

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
win_udp_socket_internal::recv_from(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    endpoint* source,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    rd_.internal_ptr = shared_from_this();

    auto& op = rd_;
    op.reset();
    op.h          = h;
    op.ex         = d;
    op.ec_out     = ec;
    op.bytes_out  = bytes_out;
    op.source_out = source;
    op.start(token);

    svc_.work_started();

    // Prepare buffers
    capy::mutable_buffer bufs[recv_from_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, recv_from_op::max_buffers));

    // Handle empty buffer: complete immediately with 0 bytes
    if (op.wsabuf_count == 0 || (op.wsabuf_count == 1 && bufs[0].size() == 0))
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

    op.flags = static_cast<DWORD>(flags);
    std::memset(&op.source_storage, 0, sizeof(op.source_storage));
    op.source_len = sizeof(op.source_storage);

    int result = ::WSARecvFrom(
        socket_, op.wsabufs, op.wsabuf_count, nullptr, &op.flags,
        reinterpret_cast<sockaddr*>(&op.source_storage), &op.source_len, &op,
        nullptr);

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

// UDP connect is synchronous on Windows
inline std::coroutine_handle<>
win_udp_socket_internal::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    conn_.internal_ptr = shared_from_this();

    auto& op = conn_;
    op.reset();
    op.h               = h;
    op.ex              = d;
    op.ec_out          = ec;
    op.target_endpoint = ep;
    op.start(token);

    svc_.work_started();

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    int result        = ::WSAConnect(
        socket_, reinterpret_cast<sockaddr*>(&storage),
        static_cast<int>(addrlen), nullptr, nullptr, nullptr, nullptr);

    if (result == SOCKET_ERROR)
        svc_.on_completion(&op, ::WSAGetLastError(), 0);
    else
        svc_.on_completion(&op, 0, 0);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_udp_socket_internal::send(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    send_wr_.internal_ptr = shared_from_this();

    auto& op = send_wr_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[udp_send_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, udp_send_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    int result = ::WSASend(
        socket_, op.wsabufs, op.wsabuf_count, nullptr,
        static_cast<DWORD>(flags), &op, nullptr);

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

    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_udp_socket_internal::recv(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    recv_rd_.internal_ptr = shared_from_this();

    auto& op = recv_rd_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[udp_recv_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, udp_recv_op::max_buffers));

    if (op.wsabuf_count == 0 || (op.wsabuf_count == 1 && bufs[0].size() == 0))
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

    op.flags = static_cast<DWORD>(flags);

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

    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_udp_socket_internal::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.internal_ptr = shared_from_this();

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

    // Datagram wait_read and wait_error route through the auxiliary
    // select reactor: there's no IOCP-native primitive for "datagram
    // readable without dequeuing the message" (zero-byte WSARecvFrom
    // would discard the next datagram), and wait_error needs the
    // reactor for the kernel-error signal in any case.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_udp_socket_internal::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }

    wr_.request_cancel();
    rd_.request_cancel();
    conn_.request_cancel();
    send_wr_.request_cancel();
    recv_rd_.request_cancel();
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);
}

inline void
win_udp_socket_internal::close_socket() noexcept
{
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    family_ = AF_UNSPEC;

    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
}

// win_udp_socket

inline win_udp_socket::win_udp_socket(
    std::shared_ptr<win_udp_socket_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_udp_socket::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_socket();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_udp_socket::send_to(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    endpoint dest,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->send_to(h, d, buf, dest, flags, token, ec, bytes);
}

inline std::coroutine_handle<>
win_udp_socket::recv_from(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    endpoint* source,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->recv_from(h, d, buf, source, flags, token, ec, bytes);
}

inline std::coroutine_handle<>
win_udp_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->connect(h, d, ep, token, ec);
}

inline std::coroutine_handle<>
win_udp_socket::send(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->send(h, d, buf, flags, token, ec, bytes);
}

inline std::coroutine_handle<>
win_udp_socket::recv(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param buf,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes)
{
    return internal_->recv(h, d, buf, flags, token, ec, bytes);
}

inline std::coroutine_handle<>
win_udp_socket::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->wait(h, d, w, token, ec);
}

inline native_handle_type
win_udp_socket::native_handle() const noexcept
{
    return static_cast<native_handle_type>(internal_->native_handle());
}

inline std::error_code
win_udp_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char const*>(data), static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_udp_socket::get_option(
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
win_udp_socket::local_endpoint() const noexcept
{
    return internal_->local_endpoint();
}

inline endpoint
win_udp_socket::remote_endpoint() const noexcept
{
    return internal_->remote_endpoint();
}

inline void
win_udp_socket::cancel() noexcept
{
    internal_->cancel();
}

inline win_udp_socket_internal*
win_udp_socket::get_internal() const noexcept
{
    return internal_.get();
}

// win_udp_service

inline win_udp_service::win_udp_service(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
}

inline win_udp_service::~win_udp_service()
{
    for (auto* w = wrapper_list_.pop_front(); w != nullptr;
         w       = wrapper_list_.pop_front())
        delete w;
}

inline void
win_udp_service::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = socket_list_.pop_front(); impl != nullptr;
         impl       = socket_list_.pop_front())
    {
        impl->close_socket();
    }
}

inline io_object::implementation*
win_udp_service::construct()
{
    auto internal = std::make_shared<win_udp_socket_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.push_back(internal.get());
    }

    auto* wrapper = new win_udp_socket(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_udp_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_udp_socket&>(*p);
        wrapper.close_internal();
        destroy_impl(wrapper);
    }
}

inline void
win_udp_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_udp_socket&>(*h.get());
    wrapper.get_internal()->close_socket();
}

inline void
win_udp_service::destroy_impl(win_udp_socket& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_udp_service::unregister_impl(win_udp_socket_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    socket_list_.remove(&impl);
}

inline std::error_code
win_udp_service::open_socket(
    win_udp_socket_internal& impl, int family, int type, int protocol)
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
win_udp_service::open_datagram_socket(
    udp_socket::implementation& impl, int family, int type, int protocol)
{
    auto& wrapper = static_cast<win_udp_socket&>(impl);
    return open_socket(*wrapper.get_internal(), family, type, protocol);
}

inline std::error_code
win_udp_service::bind_datagram(udp_socket::implementation& impl, endpoint ep)
{
    auto& wrapper  = static_cast<win_udp_socket&>(impl);
    auto* internal = wrapper.get_internal();
    SOCKET sock    = internal->socket_;

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
        internal->local_endpoint_ = detail::from_sockaddr(local_storage);

    return {};
}

inline void
win_udp_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_udp_service::on_pending(overlapped_op* op) noexcept
{
    sched_.on_pending(op);
}

inline void
win_udp_service::on_completion(
    overlapped_op* op, DWORD error, DWORD bytes) noexcept
{
    sched_.on_completion(op, error, bytes);
}

inline void
win_udp_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_udp_service::work_finished() noexcept
{
    sched_.work_finished();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_UDP_SERVICE_HPP
