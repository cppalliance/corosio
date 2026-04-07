//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_service.hpp>

#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <Ws2tcpip.h>

namespace boost::corosio::detail {

/* IOCP local stream acceptor service.

   Delegates acceptor lifecycle management to win_local_stream_service
   and provides the local_stream_acceptor_service virtual interface.
*/
class BOOST_COROSIO_DECL win_local_stream_acceptor_service final
    : public local_stream_acceptor_service
{
public:
    win_local_stream_acceptor_service(
        capy::execution_context& ctx, win_local_stream_service& svc);

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    std::error_code open_acceptor_socket(
        local_stream_acceptor::implementation& impl,
        int family, int type, int protocol) override;

    std::error_code
    bind_acceptor(
        local_stream_acceptor::implementation& impl,
        corosio::local_endpoint ep) override;

    std::error_code
    listen_acceptor(
        local_stream_acceptor::implementation& impl, int backlog) override;

    void shutdown() override;

private:
    win_local_stream_service& svc_;
};

// ============================================================
// local_stream_accept_op
// ============================================================

inline local_stream_accept_op::local_stream_accept_op() noexcept
    : overlapped_op(&do_complete)
{
    cancel_func_ = &do_cancel_impl;
}

inline void
local_stream_accept_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_accept_op*>(base);
    if (op->listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->listen_socket), op);
    }
}

// accept_op completion handler

inline void
local_stream_accept_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_accept_op*>(base);

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

        corosio::local_endpoint local_ep, remote_ep;
        if (::getsockname(
                op->accepted_socket,
                reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
            local_ep = from_sockaddr_local(
                local_storage, static_cast<socklen_t>(local_len));
        if (::getpeername(
                op->accepted_socket,
                reinterpret_cast<sockaddr*>(&remote_storage), &remote_len) == 0)
            remote_ep = from_sockaddr_local(
                remote_storage, static_cast<socklen_t>(remote_len));

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

// ============================================================
// win_local_stream_acceptor_internal
// ============================================================

inline win_local_stream_acceptor_internal::win_local_stream_acceptor_internal(
    win_local_stream_service& svc) noexcept
    : svc_(svc)
{
}

inline win_local_stream_acceptor_internal::~win_local_stream_acceptor_internal()
{
    svc_.unregister_acceptor_impl(*this);
}

inline win_local_stream_service&
win_local_stream_acceptor_internal::socket_service() noexcept
{
    return svc_;
}

inline SOCKET
win_local_stream_acceptor_internal::native_handle() const noexcept
{
    return socket_;
}

inline corosio::local_endpoint
win_local_stream_acceptor_internal::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline bool
win_local_stream_acceptor_internal::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline void
win_local_stream_acceptor_internal::set_local_endpoint(
    corosio::local_endpoint ep) noexcept
{
    local_endpoint_ = ep;
}

inline void
win_local_stream_acceptor_internal::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    acc_.request_cancel();
}

inline void
win_local_stream_acceptor_internal::close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    local_endpoint_ = corosio::local_endpoint{};
}

inline std::coroutine_handle<>
win_local_stream_acceptor_internal::accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    acc_.acceptor_ptr = shared_from_this();

    auto& op = acc_;
    op.reset();
    op.h        = h;
    op.ex       = d;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.start(token);

    svc_.work_started();

    // Create wrapper for the peer socket
    auto* peer_ptr = svc_.construct();
    if (!peer_ptr)
    {
        svc_.on_completion(&op, ERROR_OUTOFMEMORY, 0);
        return std::noop_coroutine();
    }
    auto& peer_wrapper = static_cast<win_local_stream_socket&>(*peer_ptr);

    // Always AF_UNIX for local sockets
    SOCKET accepted = ::WSASocketW(
        AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);

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

    // AcceptEx address buffer sized for sockaddr_un
    DWORD addr_size =
        static_cast<DWORD>(sizeof(un_sa_t) + 16);
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

    // Re-check cancellation after I/O is pending
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    return std::noop_coroutine();
}

// ============================================================
// win_local_stream_acceptor (wrapper)
// ============================================================

inline win_local_stream_acceptor::win_local_stream_acceptor(
    std::shared_ptr<win_local_stream_acceptor_internal> internal) noexcept
    : internal_(std::move(internal))
{
}

inline void
win_local_stream_acceptor::close_internal() noexcept
{
    if (internal_)
    {
        internal_->close_socket();
        internal_.reset();
    }
}

inline std::coroutine_handle<>
win_local_stream_acceptor::accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    return internal_->accept(h, d, token, ec, impl_out);
}

inline corosio::local_endpoint
win_local_stream_acceptor::local_endpoint() const noexcept
{
    return internal_->local_endpoint();
}

inline bool
win_local_stream_acceptor::is_open() const noexcept
{
    return internal_ && internal_->is_open();
}

inline void
win_local_stream_acceptor::cancel() noexcept
{
    if (internal_)
        internal_->cancel();
}

inline native_handle_type
win_local_stream_acceptor::release_socket() noexcept
{
    if (!internal_)
        return static_cast<native_handle_type>(INVALID_SOCKET);
    SOCKET s = internal_->socket_;
    if (s != INVALID_SOCKET)
    {
        internal_->cancel();
        internal_->socket_ = INVALID_SOCKET;
        internal_->local_endpoint_ = corosio::local_endpoint{};
    }
    return static_cast<native_handle_type>(s);
}

inline std::error_code
win_local_stream_acceptor::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (!internal_ || !internal_->is_open())
        return make_err(WSAENOTSOCK);
    if (::setsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char const*>(data), static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_local_stream_acceptor::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    if (!internal_ || !internal_->is_open())
        return make_err(WSAENOTSOCK);
    int len = static_cast<int>(*size);
    if (::getsockopt(
            internal_->native_handle(), level, optname,
            reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline win_local_stream_acceptor_internal*
win_local_stream_acceptor::get_internal() const noexcept
{
    return internal_.get();
}

// ============================================================
// win_local_stream_acceptor_service
// ============================================================

inline win_local_stream_acceptor_service::win_local_stream_acceptor_service(
    capy::execution_context& /*ctx*/, win_local_stream_service& svc)
    : svc_(svc)
{
}

inline io_object::implementation*
win_local_stream_acceptor_service::construct()
{
    auto internal =
        std::make_shared<win_local_stream_acceptor_internal>(svc_);

    // Allocate wrapper before mutating lists so a throw from
    // new doesn't leave a dangling pointer in acceptor_list_.
    auto* raw = internal.get();
    auto* wrapper = new win_local_stream_acceptor(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(svc_.mutex_);
        svc_.acceptor_list_.push_back(raw);
        svc_.acceptor_wrapper_list_.push_back(wrapper);
    }

    return wrapper;
}

inline void
win_local_stream_acceptor_service::destroy(io_object::implementation* p)
{
    if (p)
    {
        auto& wrapper = static_cast<win_local_stream_acceptor&>(*p);
        wrapper.close_internal();
        svc_.destroy_acceptor_impl(wrapper);
    }
}

inline void
win_local_stream_acceptor_service::close(io_object::handle& h)
{
    auto& wrapper = static_cast<win_local_stream_acceptor&>(*h.get());
    wrapper.get_internal()->close_socket();
}

inline std::error_code
win_local_stream_acceptor_service::open_acceptor_socket(
    local_stream_acceptor::implementation& impl,
    int family, int type, int protocol)
{
    auto* internal =
        static_cast<win_local_stream_acceptor&>(impl).get_internal();
    return svc_.open_acceptor_socket(*internal, family, type, protocol);
}

inline std::error_code
win_local_stream_acceptor_service::bind_acceptor(
    local_stream_acceptor::implementation& impl,
    corosio::local_endpoint ep)
{
    auto* internal =
        static_cast<win_local_stream_acceptor&>(impl).get_internal();
    return svc_.bind_acceptor(*internal, ep);
}

inline std::error_code
win_local_stream_acceptor_service::listen_acceptor(
    local_stream_acceptor::implementation& impl, int backlog)
{
    auto* internal =
        static_cast<win_local_stream_acceptor&>(impl).get_internal();
    return svc_.listen_acceptor(*internal, backlog);
}

inline void
win_local_stream_acceptor_service::shutdown()
{
    // Socket shutdown is handled by win_local_stream_service::shutdown()
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
