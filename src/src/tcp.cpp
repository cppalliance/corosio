//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp.hpp>
#include <boost/corosio/socket.hpp>
#include "src/detail/win_iocp_sockets.hpp"
#include "src/detail/win_iocp_scheduler.hpp"

#include <boost/corosio/detail/except.hpp>

#include <cassert>

namespace boost {
namespace corosio {

//------------------------------------------------------------------------------
// tcp::acceptor::accept_awaitable

template<capy::dispatcher Dispatcher>
auto
tcp::acceptor::accept_awaitable::
await_suspend(
    std::coroutine_handle<> h,
    Dispatcher const& d) -> std::coroutine_handle<>
{
    a_.do_accept(h, d, *peer_, token_, &ec_);
    return std::noop_coroutine();
}

// Explicit instantiation for common dispatcher
template auto tcp::acceptor::accept_awaitable::await_suspend(
    std::coroutine_handle<>, capy::any_dispatcher const&) -> std::coroutine_handle<>;

//------------------------------------------------------------------------------
// tcp::acceptor

tcp::acceptor::
~acceptor()
{
    close();
}

tcp::acceptor::
acceptor(capy::execution_context& ctx)
    : ctx_(&ctx)
    , impl_(nullptr)
{
}

tcp::acceptor::
acceptor(acceptor&& other) noexcept
    : ctx_(other.ctx_)
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
}

tcp::acceptor&
tcp::acceptor::
operator=(acceptor&& other)
{
    if (this != &other)
    {
        if (ctx_ != other.ctx_)
            detail::throw_logic_error(
                "cannot move acceptor across execution contexts");
        close();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void
tcp::acceptor::
open()
{
    if (impl_)
        return;

    auto& svc = ctx_->use_service<detail::win_iocp_sockets>();
    impl_ = &svc.create_impl();

    std::error_code ec = svc.open_socket(*impl_);
    if (ec)
    {
        impl_->release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "acceptor::open");
    }
}

void
tcp::acceptor::
bind(endpoint ep)
{
    assert(impl_ != nullptr);

    sockaddr_in addr = ep.to_sockaddr();
    if (::bind(impl_->native_handle(),
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        detail::throw_system_error(
            std::error_code(::WSAGetLastError(), std::system_category()),
            "acceptor::bind");
    }
}

void
tcp::acceptor::
listen(int backlog)
{
    assert(impl_ != nullptr);

    if (::listen(impl_->native_handle(), backlog) == SOCKET_ERROR)
    {
        detail::throw_system_error(
            std::error_code(::WSAGetLastError(), std::system_category()),
            "acceptor::listen");
    }
}

void
tcp::acceptor::
close()
{
    if (!impl_)
        return;

    impl_->release();
    impl_ = nullptr;
}

bool
tcp::acceptor::
is_open() const noexcept
{
    return impl_ != nullptr;
}

void
tcp::acceptor::
cancel()
{
    assert(impl_ != nullptr);
    impl_->cancel();
}

//------------------------------------------------------------------------------
// tcp::acceptor - static helper

void
tcp::acceptor::
accept_transfer(
    void* peer_ptr,
    void* svc_ptr,
    detail::socket_impl* impl,
    SOCKET sock)
{
    auto* peer = static_cast<socket*>(peer_ptr);
    auto* svc = static_cast<detail::win_iocp_sockets*>(svc_ptr);

    // Close any existing socket in peer
    if (peer->impl_)
    {
        peer->impl_->release();
        peer->impl_ = nullptr;
    }

    // Associate accepted socket with IOCP
    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(svc->native_handle()),
        2,  // socket_key
        0);

    if (result != nullptr)
    {
        // Enable skip completion on success
        ::SetFileCompletionNotificationModes(
            reinterpret_cast<HANDLE>(sock),
            FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

        // Transfer to peer
        impl->set_socket(sock);
        peer->impl_ = impl;
    }
    else
    {
        // IOCP association failed
        ::closesocket(sock);
        impl->release();
    }
}

void
tcp::acceptor::
do_accept(
    capy::coro h,
    capy::any_dispatcher d,
    socket& peer,
    std::stop_token token,
    std::error_code* ec)
{
    assert(impl_ != nullptr);

    auto& svc = ctx_->use_service<detail::win_iocp_sockets>();

    auto& op = impl_->acc_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.peer_impl = &svc.create_impl();
    op.peer_socket = &peer;
    op.sockets_svc = &svc;
    op.listen_socket = impl_->native_handle();
    op.transfer_fn = &acceptor::accept_transfer;
    op.start(token);

    // Create a new socket for AcceptEx
    op.accepted_socket = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (op.accepted_socket == INVALID_SOCKET)
    {
        op.peer_impl->release();
        op.peer_impl = nullptr;
        op.error = ::WSAGetLastError();
        ctx_->use_service<detail::win_iocp_scheduler>().post(&op);
        return;
    }

    // Get AcceptEx function pointer
    auto accept_ex = svc.accept_ex();
    if (!accept_ex)
    {
        ::closesocket(op.accepted_socket);
        op.accepted_socket = INVALID_SOCKET;
        op.peer_impl->release();
        op.peer_impl = nullptr;
        op.error = WSAEOPNOTSUPP;
        ctx_->use_service<detail::win_iocp_scheduler>().post(&op);
        return;
    }

    // Notify scheduler of pending I/O
    auto& sched = ctx_->use_service<detail::win_iocp_scheduler>();
    sched.work_started();

    // Start the async accept
    DWORD bytes_received = 0;
    BOOL result = accept_ex(
        impl_->native_handle(),
        op.accepted_socket,
        op.addr_buf,
        0,  // No receive data
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytes_received,
        &op);

    if (!result)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            sched.work_finished();
            ::closesocket(op.accepted_socket);
            op.accepted_socket = INVALID_SOCKET;
            op.peer_impl->release();
            op.peer_impl = nullptr;
            op.error = err;
            sched.post(&op);
            return;
        }
        // ERROR_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        sched.work_finished();
        op.error = 0;
        sched.post(&op);
    }
}

} // namespace corosio
} // namespace boost
