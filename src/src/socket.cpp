//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/socket.hpp>
#include "src/detail/win_iocp_sockets.hpp"
#include "src/detail/win_iocp_scheduler.hpp"

#include <boost/corosio/detail/except.hpp>

#include <cassert>

namespace boost {
namespace corosio {

socket::
~socket()
{
    close();
}

socket::
socket(
    capy::execution_context& ctx)
    : ctx_(&ctx)
    , impl_(nullptr)
{
}

void
socket::
open()
{
    if (impl_)
        return; // Already open

    auto& svc = ctx_->use_service<detail::win_iocp_sockets>();
    impl_ = &svc.create_impl();

    std::error_code ec = svc.open_socket(*impl_);
    if (ec)
    {
        impl_->release();
        impl_ = nullptr;
        detail::throw_system_error(ec, "socket::open");
    }
}

void
socket::
close()
{
    if (!impl_)
        return; // Already closed

    impl_->release();
    impl_ = nullptr;
}

void
socket::
cancel()
{
    assert(impl_ != nullptr);
    impl_->cancel();
}

void
socket::
do_connect(
    capy::coro h,
    capy::any_dispatcher d,
    tcp::endpoint endpoint,
    std::stop_token token,
    std::error_code* ec)
{
    assert(impl_ != nullptr);

    auto& op = impl_->conn_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.start(token);

    // ConnectEx requires the socket to be bound first
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;

    if (::bind(impl_->native_handle(),
        reinterpret_cast<sockaddr*>(&bind_addr),
        sizeof(bind_addr)) == SOCKET_ERROR)
    {
        op.error = ::WSAGetLastError();
        ctx_->use_service<detail::win_iocp_scheduler>().post(&op);
        return;
    }

    // Get the ConnectEx function pointer
    auto& svc = ctx_->use_service<detail::win_iocp_sockets>();
    auto connect_ex = svc.connect_ex();
    if (!connect_ex)
    {
        op.error = WSAEOPNOTSUPP;
        ctx_->use_service<detail::win_iocp_scheduler>().post(&op);
        return;
    }

    // Prepare the target address
    sockaddr_in addr = endpoint.to_sockaddr();

    // Notify scheduler of pending I/O
    auto& sched = ctx_->use_service<detail::win_iocp_scheduler>();
    sched.work_started();

    // Start the async connect
    BOOL result = connect_ex(
        impl_->native_handle(),
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr),
        nullptr,  // No send buffer
        0,        // No send buffer size
        nullptr,  // No bytes sent out param
        &op);

    if (!result)
    {
        DWORD err = ::WSAGetLastError();
        if (err != ERROR_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            sched.work_finished();
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

void
socket::
do_read_some(
    capy::coro h,
    capy::any_dispatcher d,
    buffers_param<true>& param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    assert(impl_ != nullptr);

    auto& op = impl_->rd_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Fill WSABUF array from the buffer sequence
    buffers::mutable_buffer bufs[detail::read_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, detail::read_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    op.flags = 0;

    // Notify scheduler of pending I/O
    auto& sched = ctx_->use_service<detail::win_iocp_scheduler>();
    sched.work_started();

    // Start the async read
    int result = ::WSARecv(
        impl_->native_handle(),
        op.wsabufs,
        op.wsabuf_count,
        nullptr,      // Bytes received (not used with overlapped)
        &op.flags,
        &op,
        nullptr);     // No completion routine

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            sched.work_finished();
            op.error = err;
            sched.post(&op);
            return;
        }
        // WSA_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        sched.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        sched.post(&op);
    }
}

void
socket::
do_write_some(
    capy::coro h,
    capy::any_dispatcher d,
    buffers_param<false>& param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    assert(impl_ != nullptr);

    auto& op = impl_->wr_;
    op.reset();
    op.h = h;
    op.d = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Fill WSABUF array from the buffer sequence
    buffers::const_buffer bufs[detail::write_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, detail::write_op::max_buffers));

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = const_cast<char*>(
            static_cast<char const*>(bufs[i].data()));
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    // Notify scheduler of pending I/O
    auto& sched = ctx_->use_service<detail::win_iocp_scheduler>();
    sched.work_started();

    // Start the async write
    int result = ::WSASend(
        impl_->native_handle(),
        op.wsabufs,
        op.wsabuf_count,
        nullptr,      // Bytes sent (not used with overlapped)
        0,            // Flags
        &op,
        nullptr);     // No completion routine

    if (result == SOCKET_ERROR)
    {
        DWORD err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // Immediate failure - no IOCP completion will occur
            sched.work_finished();
            op.error = err;
            sched.post(&op);
            return;
        }
        // WSA_IO_PENDING means the operation is in progress
    }
    else
    {
        // Synchronous completion with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
        sched.work_finished();
        op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
        op.error = 0;
        sched.post(&op);
    }
}

} // namespace corosio
} // namespace boost
