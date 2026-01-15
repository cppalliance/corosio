//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifdef _WIN32

#include "src/detail/win_iocp_sockets.hpp"
#include "src/detail/win_iocp_scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"

namespace boost {
namespace corosio {
namespace detail {

void
accept_op::
operator()()
{
    stop_cb.reset();

    bool success = (error == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = make_error_code(system::errc::operation_canceled);
        else if (error != 0)
            *ec_out = system::error_code(
                static_cast<int>(error), system::system_category());
    }

    if (success && accepted_socket != INVALID_SOCKET && peer_impl)
    {
        // Update accept context for proper socket behavior
        ::setsockopt(
            accepted_socket,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&listen_socket),
            sizeof(SOCKET));

        // Transfer socket handle to peer impl
        peer_impl->set_socket(accepted_socket);
        accepted_socket = INVALID_SOCKET;

        // Pass impl to awaitable for assignment to peer socket
        if (impl_out)
            *impl_out = peer_impl;
        peer_impl = nullptr;
    }
    else
    {
        // Cleanup on failure
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

        if (impl_out)
            *impl_out = nullptr;
    }

    d(h).resume();
}

void
accept_op::
do_cancel() noexcept
{
    if (listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(listen_socket),
            this);
    }
}

void
read_op::
do_cancel() noexcept
{
    if (impl.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(impl.native_handle()),
            this);
    }
}

void
write_op::
do_cancel() noexcept
{
    if (impl.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(impl.native_handle()),
            this);
    }
}

void
connect_op::
do_cancel() noexcept
{
    if (impl.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(impl.native_handle()),
            this);
    }
}

win_socket_impl::
win_socket_impl(win_iocp_sockets& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
{
}

void
win_socket_impl::
release()
{
    close_socket();
    svc_.destroy_impl(*this);
}

void
win_socket_impl::
connect(
    capy::any_coro h,
    capy::any_dispatcher d,
    endpoint ep,
    std::stop_token token,
    system::error_code* ec)
{
    auto& op = conn_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.start(token);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;

    if (::bind(socket_,
        reinterpret_cast<sockaddr*>(&bind_addr),
        sizeof(bind_addr)) == SOCKET_ERROR)
    {
        op.error = ::WSAGetLastError();
        svc_.post(&op);
        return;
    }

    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        op.error = WSAEOPNOTSUPP;
        svc_.post(&op);
        return;
    }

    sockaddr_in addr = detail::to_sockaddr_in(ep);

    svc_.work_started();

    BOOL result = connect_ex(
        socket_,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr),
        nullptr,
        0,
        nullptr,
        &op);

    if (!result)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        svc_.work_finished();
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_socket_impl::
read_some(
    capy::any_coro h,
    capy::any_dispatcher d,
    any_bufref& param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    capy::mutable_buffer bufs[read_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, read_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    op.flags = 0;

    svc_.work_started();

    int result = ::WSARecv(
        socket_,
        op.wsabufs,
        op.wsabuf_count,
        nullptr,
        &op.flags,
        &op,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        svc_.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_socket_impl::
write_some(
    capy::any_coro h,
    capy::any_dispatcher d,
    any_bufref& param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    capy::mutable_buffer bufs[write_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, write_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    svc_.work_started();

    int result = ::WSASend(
        socket_,
        op.wsabufs,
        op.wsabuf_count,
        nullptr,
        0,
        &op,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            svc_.work_finished();
            op.error = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        svc_.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_socket_impl::
cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
}

void
win_socket_impl::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

win_iocp_sockets::
win_iocp_sockets(
    capy::execution_context& ctx)
    : sched_(ctx.use_service<win_iocp_scheduler>())
    , iocp_(sched_.native_handle())
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
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = socket_list_.pop_front(); impl != nullptr;
         impl = socket_list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }

    for (auto* impl = acceptor_list_.pop_front(); impl != nullptr;
         impl = acceptor_list_.pop_front())
    {
        impl->close_socket();
        delete impl;
    }
}

win_socket_impl&
win_iocp_sockets::
create_impl()
{
    auto* impl = new win_socket_impl(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.push_back(impl);
    }

    return *impl;
}

void
win_iocp_sockets::
destroy_impl(win_socket_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.remove(&impl);
    }

    delete &impl;
}

system::error_code
win_iocp_sockets::
open_socket(win_socket_impl& impl)
{
    impl.close_socket();

    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
    {
        return system::error_code(
            ::WSAGetLastError(),
            system::system_category());
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        overlapped_key,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(sock);
        return system::error_code(
            static_cast<int>(err),
            system::system_category());
    }

    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(sock),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    impl.socket_ = sock;
    return {};
}

void
win_iocp_sockets::
post(overlapped_op* op)
{
    sched_.post(op);
}

void
win_iocp_sockets::
work_started() noexcept
{
    sched_.work_started();
}

void
win_iocp_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

void
win_iocp_sockets::
load_extension_functions()
{
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

win_acceptor_impl&
win_iocp_sockets::
create_acceptor_impl()
{
    auto* impl = new win_acceptor_impl(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_list_.push_back(impl);
    }

    return *impl;
}

void
win_iocp_sockets::
destroy_acceptor_impl(win_acceptor_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_list_.remove(&impl);
    }

    delete &impl;
}

system::error_code
win_iocp_sockets::
open_acceptor(
    win_acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    impl.close_socket();

    SOCKET sock = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
    {
        return system::error_code(
            ::WSAGetLastError(),
            system::system_category());
    }

    // Allow address reuse
    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<char*>(&reuse), sizeof(reuse));

    // Register with IOCP
    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        overlapped_key,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(sock);
        return system::error_code(
            static_cast<int>(err),
            system::system_category());
    }

    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(sock),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    // Bind to endpoint
    sockaddr_in addr = detail::to_sockaddr_in(ep);
    if (::bind(sock,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        ::closesocket(sock);
        return system::error_code(
            static_cast<int>(err),
            system::system_category());
    }

    // Start listening
    if (::listen(sock, backlog) == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        ::closesocket(sock);
        return system::error_code(
            static_cast<int>(err),
            system::system_category());
    }

    impl.socket_ = sock;
    return {};
}

//------------------------------------------------------------------------------
// win_acceptor_impl
//------------------------------------------------------------------------------

win_acceptor_impl::
win_acceptor_impl(win_iocp_sockets& svc) noexcept
    : svc_(svc)
{
}

void
win_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

void
win_acceptor_impl::
accept(
    capy::any_coro h,
    capy::any_dispatcher d,
    std::stop_token token,
    system::error_code* ec,
    io_object::io_object_impl** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.impl_out = impl_out;
    op.start(token);

    // Create impl for the peer socket
    auto& peer_impl = svc_.create_impl();

    // Create the accepted socket
    SOCKET accepted = ::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0,
        WSA_FLAG_OVERLAPPED);

    if (accepted == INVALID_SOCKET)
    {
        peer_impl.release();
        op.error = ::WSAGetLastError();
        svc_.post(&op);
        return;
    }

    // Register accepted socket with IOCP
    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(accepted),
        svc_.native_handle(),
        overlapped_key,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(accepted);
        peer_impl.release();
        op.error = err;
        svc_.post(&op);
        return;
    }

    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(accepted),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    // Set up the accept operation
    op.accepted_socket = accepted;
    op.peer_impl = &peer_impl;
    op.listen_socket = socket_;

    auto accept_ex = svc_.accept_ex();
    if (!accept_ex)
    {
        ::closesocket(accepted);
        peer_impl.release();
        op.accepted_socket = INVALID_SOCKET;
        op.peer_impl = nullptr;
        op.error = WSAEOPNOTSUPP;
        svc_.post(&op);
        return;
    }

    DWORD bytes_received = 0;
    svc_.work_started();

    BOOL ok = accept_ex(
        socket_,
        accepted,
        op.addr_buf,
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytes_received,
        &op);

    if (!ok)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            svc_.work_finished();
            ::closesocket(accepted);
            peer_impl.release();
            op.accepted_socket = INVALID_SOCKET;
            op.peer_impl = nullptr;
            op.error = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        svc_.work_finished();
        op.error = 0;
        svc_.post(&op);
    }
}

void
win_acceptor_impl::
cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }

    acc_.request_cancel();
}

void
win_acceptor_impl::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // _WIN32
