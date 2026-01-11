//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/win_iocp_sockets.hpp"

#ifdef _WIN32

#include "src/detail/win_iocp_scheduler.hpp"

#include <Ws2tcpip.h>

namespace boost {
namespace corosio {
namespace detail {

namespace {

// Completion key for socket I/O operations
constexpr ULONG_PTR socket_key = 2;

} // namespace

//------------------------------------------------------------------------------
// socket_impl

//------------------------------------------------------------------------------
// accept_op

void
accept_op::
operator()()
{
    stop_cb.reset();

    bool success = (error == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = std::make_error_code(std::errc::operation_canceled);
        else if (error != 0)
            *ec_out = std::error_code(
                static_cast<int>(error), std::system_category());
    }

    // Transfer accepted socket on success
    if (success && transfer_fn && peer_socket && sockets_svc && 
        accepted_socket != INVALID_SOCKET)
    {
        // Update accepted socket context
        ::setsockopt(
            accepted_socket,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&listen_socket),
            sizeof(SOCKET));

        // Call the transfer function to set up peer
        transfer_fn(peer_socket, sockets_svc, peer_impl, accepted_socket);
        accepted_socket = INVALID_SOCKET;
        peer_impl = nullptr;
    }
    else
    {
        // Clean up on failure
        if (accepted_socket != INVALID_SOCKET)
        {
            ::closesocket(accepted_socket);
            accepted_socket = INVALID_SOCKET;
        }

        if (peer_impl)
        {
            peer_impl->release();
            peer_impl = nullptr;
        }
    }

    d(h).resume();
}

//------------------------------------------------------------------------------
// socket_impl

void
socket_impl::
cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        // Cancel all pending I/O on this socket
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }

    // Mark operations as cancelled
    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    acc_.request_cancel();
}

void
socket_impl::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void
socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

//------------------------------------------------------------------------------
// win_iocp_sockets

win_iocp_sockets::
win_iocp_sockets(
    capy::execution_context& ctx)
    : iocp_(ctx.use_service<win_iocp_scheduler>().native_handle())
{
    load_extension_functions();
}

win_iocp_sockets::
~win_iocp_sockets()
{
}

void
win_iocp_sockets::
shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Destroy all socket implementations
    for (auto* impl = list_.pop_front(); impl != nullptr;
         impl = list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }
}

socket_impl&
win_iocp_sockets::
create_impl()
{
    auto* impl = new socket_impl(*this);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.push_back(impl);
    }

    return *impl;
}

void
win_iocp_sockets::
destroy_impl(socket_impl& impl)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        list_.remove(&impl);
    }

    delete &impl;
}

std::error_code
win_iocp_sockets::
open_socket(socket_impl& impl)
{
    // Close existing socket if any
    impl.close_socket();

    // Create an overlapped IPv4 TCP socket
    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
    {
        return std::error_code(
            ::WSAGetLastError(),
            std::system_category());
    }

    // Associate the socket with the IOCP
    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        socket_key,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(sock);
        return std::error_code(
            static_cast<int>(err),
            std::system_category());
    }

    // Disable IOCP notification for synchronous completions
    // This prevents spurious completions when operations complete inline
    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(sock),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    impl.socket_ = sock;
    return {};
}

void
win_iocp_sockets::
load_extension_functions()
{
    // Create a temporary socket to load extension functions
    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return;

    DWORD bytes = 0;

    // Load ConnectEx
    GUID connect_ex_guid = WSAID_CONNECTEX;
    ::WSAIoctl(
        sock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &connect_ex_guid,
        sizeof(connect_ex_guid),
        &connect_ex_,
        sizeof(connect_ex_),
        &bytes,
        nullptr,
        nullptr);

    // Load AcceptEx
    GUID accept_ex_guid = WSAID_ACCEPTEX;
    ::WSAIoctl(
        sock,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &accept_ex_guid,
        sizeof(accept_ex_guid),
        &accept_ex_,
        sizeof(accept_ex_),
        &bytes,
        nullptr,
        nullptr);

    ::closesocket(sock);
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32
