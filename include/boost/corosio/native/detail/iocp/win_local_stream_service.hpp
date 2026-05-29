//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>

#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_socket.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_service.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <Ws2tcpip.h>

namespace boost::corosio::detail {

class win_local_stream_acceptor;
class win_local_stream_acceptor_internal;
class win_local_stream_acceptor_service;

/* IOCP local stream socket service.

   Inherits from local_stream_service to enable runtime polymorphism
   via use_service<local_stream_service>(). Reuses the ConnectEx /
   AcceptEx function pointers already loaded by win_tcp_service.
*/
class BOOST_COROSIO_DECL win_local_stream_service final
    : private win_wsa_init
    , public local_stream_service
{
public:
    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    explicit win_local_stream_service(
        capy::execution_context& ctx, win_tcp_service& tcp_svc);

    ~win_local_stream_service();

    win_local_stream_service(win_local_stream_service const&)            = delete;
    win_local_stream_service& operator=(win_local_stream_service const&) = delete;

    void shutdown() override;

    std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family, int type, int protocol) override;

    std::error_code assign_socket(
        local_stream_socket::implementation& impl,
        native_handle_type fd) override;

    void destroy_impl(win_local_stream_socket& impl);

    void unregister_impl(win_local_stream_socket_internal& impl);

    std::error_code open_socket_internal(
        win_local_stream_socket_internal& impl,
        int family, int type, int protocol);

    void destroy_acceptor_impl(win_local_stream_acceptor& impl);

    void unregister_acceptor_impl(win_local_stream_acceptor_internal& impl);

    std::error_code open_acceptor_socket(
        win_local_stream_acceptor_internal& impl,
        int family, int type, int protocol);

    std::error_code bind_acceptor(
        win_local_stream_acceptor_internal& impl,
        corosio::local_endpoint ep);

    std::error_code listen_acceptor(
        win_local_stream_acceptor_internal& impl, int backlog);

    void* native_handle() const noexcept;
    LPFN_CONNECTEX connect_ex() const noexcept;
    LPFN_ACCEPTEX accept_ex() const noexcept;

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
    friend class win_local_stream_acceptor_service;

    win_tcp_service& tcp_svc_;
    win_scheduler& sched_;
    win_mutex mutex_;
    intrusive_list<win_local_stream_socket_internal> socket_list_;
    intrusive_list<win_local_stream_acceptor_internal> acceptor_list_;
    intrusive_list<win_local_stream_socket> socket_wrapper_list_;
    intrusive_list<win_local_stream_acceptor> acceptor_wrapper_list_;
    void* iocp_;
};

// ============================================================
// Operation constructors
// ============================================================

inline local_stream_connect_op::local_stream_connect_op(
    win_local_stream_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline local_stream_read_op::local_stream_read_op(
    win_local_stream_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline local_stream_write_op::local_stream_write_op(
    win_local_stream_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline local_stream_wait_op::local_stream_wait_op(
    win_local_stream_socket_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

// ============================================================
// Cancellation functions
// ============================================================

inline void
local_stream_connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_connect_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
local_stream_read_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_read_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
local_stream_write_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_write_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
}

inline void
local_stream_wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()), op);
    }
    op->internal.svc_.scheduler().cancel_wait_if_constructed(op);
}

// ============================================================
// connect_op completion handler
// ============================================================

inline void
local_stream_connect_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_connect_op*>(base);

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
        // Required after ConnectEx
        ::setsockopt(
            op->internal.native_handle(), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
            nullptr, 0);

        corosio::local_endpoint local_ep;
        sockaddr_storage local_storage{};
        int local_len = sizeof(local_storage);
        if (::getsockname(
                op->internal.native_handle(),
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            local_ep = from_sockaddr_local(
                local_storage, static_cast<socklen_t>(local_len));
        op->internal.set_endpoints(local_ep, op->target_endpoint);
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// ============================================================
// read_op completion handler
// ============================================================

inline void
local_stream_read_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_read_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// ============================================================
// write_op completion handler
// ============================================================

inline void
local_stream_write_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_write_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// ============================================================
// wait_op completion handler
// ============================================================

inline void
local_stream_wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_wait_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->internal_ptr.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

// ============================================================
// win_local_stream_socket_internal
// ============================================================

inline win_local_stream_socket_internal::win_local_stream_socket_internal(
    win_local_stream_service& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
    , wt_(*this)
{
}

inline win_local_stream_socket_internal::~win_local_stream_socket_internal()
{
    svc_.unregister_impl(*this);
}

inline SOCKET
win_local_stream_socket_internal::native_handle() const noexcept
{
    return socket_;
}

inline corosio::local_endpoint
win_local_stream_socket_internal::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline corosio::local_endpoint
win_local_stream_socket_internal::remote_endpoint() const noexcept
{
    return remote_endpoint_;
}

inline bool
win_local_stream_socket_internal::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline void
win_local_stream_socket_internal::set_socket(SOCKET s) noexcept
{
    socket_ = s;
}

inline void
win_local_stream_socket_internal::set_endpoints(
    corosio::local_endpoint local,
    corosio::local_endpoint remote) noexcept
{
    local_endpoint_  = local;
    remote_endpoint_ = remote;
}

inline std::coroutine_handle<>
win_local_stream_socket_internal::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    corosio::local_endpoint ep,
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

    // ConnectEx requires the socket to be bound. For AF_UNIX,
    // bind to a family-only sockaddr_un (empty path).
    if (local_endpoint_.empty())
    {
        un_sa_t bind_sa{};
        bind_sa.sun_family = AF_UNIX;
        socklen_t bind_len =
            static_cast<socklen_t>(offsetof(un_sa_t, sun_path));

        if (::bind(
                socket_, reinterpret_cast<sockaddr*>(&bind_sa),
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
    socklen_t addrlen = detail::to_sockaddr(ep, storage);

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

    // Re-check cancellation after I/O is pending
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_local_stream_socket_internal::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    rd_.internal_ptr = shared_from_this();

    auto& op = rd_;
    op.reset();
    op.is_read_  = true;
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[local_stream_read_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, local_stream_read_op::max_buffers));

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

    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_local_stream_socket_internal::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    wr_.internal_ptr = shared_from_this();

    auto& op = wr_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    svc_.work_started();

    capy::mutable_buffer bufs[local_stream_write_op::max_buffers];
    op.wsabuf_count =
        static_cast<DWORD>(param.copy_to(bufs, local_stream_write_op::max_buffers));

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

    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_local_stream_socket_internal::wait(
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
    op.empty_buffer = true;
    op.start(token);

    svc_.work_started();

    if (w == wait_type::write)
    {
        svc_.on_completion(&op, 0, 0);
        return std::noop_coroutine();
    }

    if (w == wait_type::read)
    {
        // Zero-byte WSARecv — completes when data is available
        // without consuming any bytes.
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

        if (op.cancelled.load(std::memory_order_acquire))
            ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

        return std::noop_coroutine();
    }

    // wait_type::error: route through the auxiliary select reactor.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_local_stream_socket_internal::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);
}

inline void
win_local_stream_socket_internal::close_socket() noexcept
{
    wt_.request_cancel();
    svc_.scheduler().cancel_wait_if_constructed(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    local_endpoint_  = corosio::local_endpoint{};
    remote_endpoint_ = corosio::local_endpoint{};
}

// ============================================================
// win_local_stream_socket (wrapper)
// ============================================================

inline win_local_stream_socket::win_local_stream_socket(
    std::shared_ptr<win_local_stream_socket_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_local_stream_socket::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_socket();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_local_stream_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    corosio::local_endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->connect(h, d, ep, token, ec);
}

inline std::coroutine_handle<>
win_local_stream_socket::read_some(
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
win_local_stream_socket::write_some(
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
win_local_stream_socket::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    return internal_->wait(h, d, w, token, ec);
}

inline std::error_code
win_local_stream_socket::shutdown(
    local_stream_socket::shutdown_type what) noexcept
{
    int how;
    switch (what)
    {
    case local_stream_socket::shutdown_receive:
        how = SD_RECEIVE;
        break;
    case local_stream_socket::shutdown_send:
        how = SD_SEND;
        break;
    case local_stream_socket::shutdown_both:
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
win_local_stream_socket::native_handle() const noexcept
{
    return static_cast<native_handle_type>(internal_->native_handle());
}

inline native_handle_type
win_local_stream_socket::release_socket() noexcept
{
    SOCKET s = internal_->socket_;
    if (s != INVALID_SOCKET)
    {
        internal_->cancel();
        internal_->socket_ = INVALID_SOCKET;
        internal_->local_endpoint_  = corosio::local_endpoint{};
        internal_->remote_endpoint_ = corosio::local_endpoint{};
    }
    return static_cast<native_handle_type>(s);
}

inline std::error_code
win_local_stream_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char const*>(data), static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_local_stream_socket::get_option(
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

inline corosio::local_endpoint
win_local_stream_socket::local_endpoint() const noexcept
{
    return internal_->local_endpoint();
}

inline corosio::local_endpoint
win_local_stream_socket::remote_endpoint() const noexcept
{
    return internal_->remote_endpoint();
}

inline void
win_local_stream_socket::cancel() noexcept
{
    internal_->cancel();
}

inline win_local_stream_socket_internal*
win_local_stream_socket::get_internal() const noexcept
{
    return internal_.get();
}

// ============================================================
// win_local_stream_service
// ============================================================

inline win_local_stream_service::win_local_stream_service(
    capy::execution_context& ctx, win_tcp_service& tcp_svc)
    : tcp_svc_(tcp_svc)
    , sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
}

inline win_local_stream_service::~win_local_stream_service()
{
    for (auto* w = socket_wrapper_list_.pop_front(); w != nullptr;
         w       = socket_wrapper_list_.pop_front())
        delete w;

    for (auto* w = acceptor_wrapper_list_.pop_front(); w != nullptr;
         w       = acceptor_wrapper_list_.pop_front())
        delete w;
}

inline void
win_local_stream_service::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

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
win_local_stream_service::construct()
{
    auto internal = std::make_shared<win_local_stream_socket_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.push_back(internal.get());
    }

    auto* wrapper = new win_local_stream_socket(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_local_stream_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_local_stream_socket&>(*p);
        wrapper.close_internal();
        destroy_impl(wrapper);
    }
}

inline void
win_local_stream_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_local_stream_socket&>(*h.get());
    wrapper.get_internal()->close_socket();
}

inline void
win_local_stream_service::destroy_impl(win_local_stream_socket& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_local_stream_service::unregister_impl(
    win_local_stream_socket_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    socket_list_.remove(&impl);
}

inline std::error_code
win_local_stream_service::open_socket(
    local_stream_socket::implementation& impl,
    int family, int type, int protocol)
{
    auto& wrapper = static_cast<win_local_stream_socket&>(impl);
    return open_socket_internal(*wrapper.get_internal(), family, type, protocol);
}

inline std::error_code
win_local_stream_service::assign_socket(
    local_stream_socket::implementation& impl, native_handle_type fd)
{
    auto& wrapper = static_cast<win_local_stream_socket&>(impl);
    auto& internal = *wrapper.get_internal();

    internal.close_socket();

    SOCKET sock = static_cast<SOCKET>(fd);

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        return make_err(dwError);
    }

    internal.socket_ = sock;
    return {};
}

inline std::error_code
win_local_stream_service::open_socket_internal(
    win_local_stream_socket_internal& impl,
    int family, int type, int protocol)
{
    impl.close_socket();

    SOCKET sock =
        ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return make_err(::WSAGetLastError());

    // No IPV6_V6ONLY for AF_UNIX

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

inline void*
win_local_stream_service::native_handle() const noexcept
{
    return iocp_;
}

inline LPFN_CONNECTEX
win_local_stream_service::connect_ex() const noexcept
{
    return tcp_svc_.connect_ex();
}

inline LPFN_ACCEPTEX
win_local_stream_service::accept_ex() const noexcept
{
    return tcp_svc_.accept_ex();
}

inline void
win_local_stream_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_local_stream_service::on_pending(overlapped_op* op) noexcept
{
    sched_.on_pending(op);
}

inline void
win_local_stream_service::on_completion(
    overlapped_op* op, DWORD error, DWORD bytes) noexcept
{
    sched_.on_completion(op, error, bytes);
}

inline void
win_local_stream_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_local_stream_service::work_finished() noexcept
{
    sched_.work_finished();
}

inline void
win_local_stream_service::destroy_acceptor_impl(
    win_local_stream_acceptor& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

inline void
win_local_stream_service::unregister_acceptor_impl(
    win_local_stream_acceptor_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    acceptor_list_.remove(&impl);
}

inline std::error_code
win_local_stream_service::open_acceptor_socket(
    win_local_stream_acceptor_internal& impl,
    int family, int type, int protocol)
{
    impl.close_socket();

    SOCKET sock =
        ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return make_err(::WSAGetLastError());

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
win_local_stream_service::bind_acceptor(
    win_local_stream_acceptor_internal& impl,
    corosio::local_endpoint ep)
{
    // Reject abstract sockets on Windows
    if (ep.is_abstract())
        return std::make_error_code(std::errc::operation_not_supported);

    SOCKET sock = impl.socket_;

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, storage);
    if (::bind(
            sock, reinterpret_cast<sockaddr*>(&storage),
            static_cast<int>(addrlen)) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    impl.set_local_endpoint(ep);
    return {};
}

inline std::error_code
win_local_stream_service::listen_acceptor(
    win_local_stream_acceptor_internal& impl, int backlog)
{
    SOCKET sock = impl.socket_;

    if (::listen(sock, backlog) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_SERVICE_HPP
