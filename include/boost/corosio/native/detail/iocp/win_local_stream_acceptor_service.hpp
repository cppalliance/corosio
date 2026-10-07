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

#include <boost/corosio/native/detail/iocp/win_dissociate.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_acceptor.hpp>
#include <boost/corosio/native/detail/iocp/win_local_stream_service.hpp>

#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>

#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <ws2tcpip.h>

namespace boost::corosio::detail {

/* IOCP local stream acceptor service.

   Delegates acceptor lifecycle management to win_local_stream_service
   and provides the local_stream_acceptor_service virtual interface.
*/
class BOOST_COROSIO_DECL win_local_stream_acceptor_service final
    : public local_stream_acceptor_service
{
public:
    explicit win_local_stream_acceptor_service(capy::execution_context& ctx);

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    std::error_code open_acceptor_socket(
        local_stream_acceptor::implementation& impl,
        int family,
        int type,
        int protocol) override;

    std::error_code assign_socket(
        local_stream_acceptor::implementation& impl,
        native_handle_type fd) override;

    std::error_code bind_acceptor(
        local_stream_acceptor::implementation& impl,
        corosio::local_endpoint ep) override;

    std::error_code listen_acceptor(
        local_stream_acceptor::implementation& impl, int backlog) override;

    void shutdown() override;

private:
    win_local_stream_service& svc_;
};

// ============================================================
// local_stream_accept_op
// ============================================================

inline local_stream_accept_op::local_stream_accept_op(
    win_local_stream_acceptor& acceptor_) noexcept
    : overlapped_op(&do_complete)
    , acceptor(acceptor_)
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

inline local_stream_acceptor_wait_op::local_stream_acceptor_wait_op(
    win_local_stream_acceptor& acceptor_) noexcept
    : overlapped_op(&do_complete)
    , acceptor(acceptor_)
{
    cancel_func_ = &do_cancel_impl;
}

inline void
local_stream_acceptor_wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<local_stream_acceptor_wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->listen_socket), op);
    }
    op->acceptor.socket_service().scheduler().cancel_wait(op);
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
            op->acceptor.socket_service().destroy(op->peer_wrapper);
            op->peer_wrapper = nullptr;
        }

        op->cleanup_only();
        op->object_ref_.reset();
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
            *op->ec_out = iocp_make_err(op->dwError, /*accept_path=*/true);
        else
            *op->ec_out = {};
    }

    if (success && op->accepted_socket != INVALID_SOCKET && op->peer_wrapper)
    {
        ::setsockopt(
            op->accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&op->listen_socket), sizeof(SOCKET));

        op->peer_wrapper->set_socket(op->accepted_socket);

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

        op->peer_wrapper->set_endpoints(local_ep, remote_ep);
        op->accepted_socket = INVALID_SOCKET;

        if (op->impl_out)
            *op->impl_out = op->peer_wrapper;
        // Handed off to the caller: don't let a failed early return on
        // the next accept() see this (now user-owned) pointer again.
        op->peer_wrapper = nullptr;
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
            op->acceptor.socket_service().destroy(op->peer_wrapper);
            op->peer_wrapper = nullptr;
        }

        if (op->impl_out)
            *op->impl_out = nullptr;
    }

    op->cont.h                         = op->h;
    auto saved_ex                      = op->ex;
    auto prevent_premature_destruction = std::move(op->object_ref_);

    dispatch_coro(saved_ex, op->cont).resume();
}

inline void
local_stream_acceptor_wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<local_stream_acceptor_wait_op*>(base);

    if (!owner)
    {
        op->cleanup_only();
        op->object_ref_.reset();
        return;
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// ============================================================
// win_local_stream_acceptor
// ============================================================

inline win_local_stream_acceptor::win_local_stream_acceptor(
    win_local_stream_service& svc) noexcept
    : svc_(svc)
    , acc_(*this)
    , wt_(*this)
{
}

inline void
win_local_stream_acceptor::reuse() noexcept
{
    BOOST_COROSIO_ASSERT(socket_ == INVALID_SOCKET);
    BOOST_COROSIO_ASSERT(local_endpoint_ == corosio::local_endpoint{});
    BOOST_COROSIO_ASSERT(!acc_.stop_cb);
    BOOST_COROSIO_ASSERT(!wt_.stop_cb);
    BOOST_COROSIO_ASSERT(acc_.peer_wrapper == nullptr);
    BOOST_COROSIO_ASSERT(acc_.accepted_socket == INVALID_SOCKET);
}

inline win_local_stream_service&
win_local_stream_acceptor::socket_service() noexcept
{
    return svc_;
}

inline native_handle_type
win_local_stream_acceptor::native_handle() const noexcept
{
    return static_cast<native_handle_type>(socket_);
}

inline corosio::local_endpoint
win_local_stream_acceptor::local_endpoint() const noexcept
{
    return local_endpoint_;
}

inline bool
win_local_stream_acceptor::is_open() const noexcept
{
    return socket_ != INVALID_SOCKET;
}

inline void
win_local_stream_acceptor::set_local_endpoint(
    corosio::local_endpoint ep) noexcept
{
    local_endpoint_ = ep;
}

inline void
win_local_stream_acceptor::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    acc_.request_cancel();
    wt_.request_cancel();
    svc_.scheduler().cancel_wait(&wt_);
}

inline std::coroutine_handle<>
win_local_stream_acceptor::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.object_ref_   = detail::object_ref(this);
    wt_.listen_socket = socket_;

    auto& op = wt_;
    op.reset();
    op.h         = h;
    op.ex        = d;
    op.ec_out    = ec;
    op.bytes_out = nullptr;
    op.start(token);

    svc_.work_started();

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel or the wait reactor.
    if (socket_ == INVALID_SOCKET)
    {
        svc_.on_completion(&op, WSAEBADF, 0);
        return std::noop_coroutine();
    }

    // Writability carries no meaning for a listening socket; the
    // wait fails the same way on every backend.
    if (w == wait_type::write)
    {
        svc_.on_completion(&op, WSAEOPNOTSUPP, 0);
        return std::noop_coroutine();
    }

    // wait_type::read and wait_type::error route through the auxiliary
    // select reactor.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_local_stream_acceptor::close_socket() noexcept
{
    // Flag the accept op cancelled before closing so a closesocket-delivered
    // ERROR_NETNAME_DELETED is short-circuited to canceled rather than mapped
    // to connection_aborted by iocp_make_err (see win_tcp_socket close_socket).
    acc_.request_cancel();
    wt_.request_cancel();
    svc_.scheduler().cancel_wait(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    local_endpoint_ = corosio::local_endpoint{};
}

inline std::coroutine_handle<>
win_local_stream_acceptor::accept(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    acc_.object_ref_ = detail::object_ref(this);

    auto& op = acc_;
    op.reset();
    // reset() doesn't touch these -- a value left over from a prior
    // accept() (e.g. the socket already handed to the user via
    // *impl_out) must not survive into a completion this call fails
    // before reassigning; do_complete's failure branch would destroy()
    // whatever stale pointer it finds here.
    op.peer_wrapper    = nullptr;
    op.accepted_socket = INVALID_SOCKET;
    op.h               = h;
    op.ex              = d;
    op.ec_out          = ec;
    op.impl_out        = impl_out;
    op.start(token);

    svc_.work_started();

    // Create a (possibly recycled) peer socket
    auto* peer_ptr = svc_.construct();
    if (!peer_ptr)
    {
        svc_.on_completion(&op, ERROR_OUTOFMEMORY, 0);
        return std::noop_coroutine();
    }
    auto& peer_wrapper = static_cast<win_local_stream_socket&>(*peer_ptr);

    // Always AF_UNIX for local sockets
    SOCKET accepted =
        ::WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);

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
    DWORD addr_size      = static_cast<DWORD>(sizeof(un_sa_t) + 16);
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

inline native_handle_type
win_local_stream_acceptor::release_socket() noexcept
{
    SOCKET s = socket_;
    if (s != INVALID_SOCKET)
    {
        cancel();
        dissociate_from_iocp(s);
        socket_         = INVALID_SOCKET;
        local_endpoint_ = corosio::local_endpoint{};
    }
    return static_cast<native_handle_type>(s);
}

inline std::error_code
win_local_stream_acceptor::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (socket_ == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);
    if (::setsockopt(
            socket_, level, optname, reinterpret_cast<char const*>(data),
            static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_local_stream_acceptor::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    if (socket_ == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);
    int len = static_cast<int>(*size);
    if (::getsockopt(
            socket_, level, optname, reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline void
win_local_stream_acceptor::retire() noexcept
{
    svc_.acceptor_pool_.recycle(this);
}

// ============================================================
// win_local_stream_acceptor_service
// ============================================================

inline win_local_stream_acceptor_service::win_local_stream_acceptor_service(
    capy::execution_context& ctx)
    : svc_(ctx.use_service<win_local_stream_service>())
{
}

inline io_object::implementation*
win_local_stream_acceptor_service::construct()
{
    return svc_.acquire_acceptor_impl();
}

inline void
win_local_stream_acceptor_service::destroy(io_object::implementation* p)
{
    if (!p)
        return;
    auto* a = static_cast<win_local_stream_acceptor*>(p);
    a->close_socket();
    release(a);
}

inline void
win_local_stream_acceptor_service::close(io_object::handle& h)
{
    static_cast<win_local_stream_acceptor*>(h.get())->close_socket();
}

inline std::error_code
win_local_stream_acceptor_service::open_acceptor_socket(
    local_stream_acceptor::implementation& impl,
    int family,
    int type,
    int protocol)
{
    auto& acc = static_cast<win_local_stream_acceptor&>(impl);
    return svc_.open_acceptor_socket(acc, family, type, protocol);
}

inline std::error_code
win_local_stream_acceptor_service::assign_socket(
    local_stream_acceptor::implementation& impl, native_handle_type fd)
{
    auto& acc = static_cast<win_local_stream_acceptor&>(impl);
    return svc_.assign_acceptor_socket(acc, fd);
}

inline std::error_code
win_local_stream_acceptor_service::bind_acceptor(
    local_stream_acceptor::implementation& impl, corosio::local_endpoint ep)
{
    auto& acc = static_cast<win_local_stream_acceptor&>(impl);
    return svc_.bind_acceptor(acc, ep);
}

inline std::error_code
win_local_stream_acceptor_service::listen_acceptor(
    local_stream_acceptor::implementation& impl, int backlog)
{
    auto& acc = static_cast<win_local_stream_acceptor&>(impl);
    return svc_.listen_acceptor(acc, backlog);
}

inline void
win_local_stream_acceptor_service::shutdown()
{
    // Socket shutdown is handled by win_local_stream_service::shutdown()
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_LOCAL_STREAM_ACCEPTOR_SERVICE_HPP
