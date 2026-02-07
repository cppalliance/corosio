//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/sockets.hpp"
#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/resume_coro.hpp"

/*
    Windows IOCP Socket Implementation
    ==================================

    Uses function pointer dispatch instead of virtual dispatch.
    All socket handles are registered with IOCP using key_io (0).
    Each operation type has a static do_complete function.
*/

namespace boost::corosio::detail {

//------------------------------------------------------------------------------
// Operation constructors

connect_op::connect_op(win_socket_impl_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

read_op::read_op(win_socket_impl_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

write_op::write_op(win_socket_impl_internal& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

accept_op::accept_op() noexcept
    : overlapped_op(&do_complete)
{
    cancel_func_ = &do_cancel_impl;
}

//------------------------------------------------------------------------------
// Cancellation functions

void connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<connect_op*>(base);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()),
            op);
    }
}

void read_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<read_op*>(base);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()),
            op);
    }
}

void write_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<write_op*>(base);
    if (op->internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->internal.native_handle()),
            op);
    }
}

void accept_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<accept_op*>(base);
    if (op->listen_socket != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(op->listen_socket),
            op);
    }
}

//------------------------------------------------------------------------------
// accept_op completion handler

void
accept_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<accept_op*>(base);

    // Destroy path
    if (!owner)
    {
        op->cleanup_only();
        return;
    }

    op->stop_cb.reset();

    bool success = (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));

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
            op->accepted_socket,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&op->listen_socket),
            sizeof(SOCKET));

        op->peer_wrapper->get_internal()->set_socket(op->accepted_socket);

        sockaddr_in local_addr{};
        int local_len = sizeof(local_addr);
        sockaddr_in remote_addr{};
        int remote_len = sizeof(remote_addr);

        endpoint local_ep, remote_ep;
        if (::getsockname(op->accepted_socket,
            reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        if (::getpeername(op->accepted_socket,
            reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0)
            remote_ep = from_sockaddr_in(remote_addr);

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
            op->peer_wrapper->release();
            op->peer_wrapper = nullptr;
        }

        if (op->impl_out)
            *op->impl_out = nullptr;
    }

    auto saved_h = op->h;
    auto saved_ex = op->ex;
    auto prevent_premature_destruction = std::move(op->acceptor_ptr);

    resume_coro(saved_ex, saved_h);
}

//------------------------------------------------------------------------------
// connect_op completion handler

void
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

    bool success = (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success && op->internal.is_open())
    {
        endpoint local_ep;
        sockaddr_in local_addr{};
        int local_len = sizeof(local_addr);
        if (::getsockname(op->internal.native_handle(),
            reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        op->internal.set_endpoints(local_ep, op->target_endpoint);
    }

    auto prevent_premature_destruction = std::move(op->internal_ptr);
    op->invoke_handler();
}

//------------------------------------------------------------------------------
// read_op completion handler

void
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

//------------------------------------------------------------------------------
// write_op completion handler

void
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

win_socket_impl_internal::
win_socket_impl_internal(win_sockets& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
{
}

win_socket_impl_internal::
~win_socket_impl_internal()
{
    svc_.unregister_impl(*this);
}

void
win_socket_impl_internal::
release_internal()
{
    // Cancel pending I/O before closing to ensure operations
    // complete with ERROR_OPERATION_ABORTED via IOCP
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }
    close_socket();
}

std::coroutine_handle<>
win_socket_impl_internal::
connect(
    capy::coro h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    // Keep internal alive during I/O
    conn_.internal_ptr = shared_from_this();

    auto& op = conn_;
    op.reset();
    op.h = h;
    op.ex = d;
    op.ec_out = ec;
    op.target_endpoint = ep;  // Store target for endpoint caching
    op.start(token);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;

    if (::bind(socket_,
        reinterpret_cast<sockaddr*>(&bind_addr),
        sizeof(bind_addr)) == SOCKET_ERROR)
    {
        op.dwError = ::WSAGetLastError();
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        op.dwError = WSAEOPNOTSUPP;
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
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
            op.dwError = err;
            svc_.post(&op);
            // completion is always posted to scheduler queue, never inline.
            return std::noop_coroutine();
        }
    }
    // Synchronous completion: IOCP will deliver the completion packet
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

//------------------------------------------------------------------------------

void
win_socket_impl_internal::
do_read_io()
{
    auto& op = rd_;

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
            // Sync failure - release internal_ptr before resuming
            svc_.work_finished();
            op.dwError = err;
            auto prevent_premature_destruction = std::move(op.internal_ptr);
            op.invoke_handler();
            return;
        }
    }
    // Synchronous completion: IOCP will deliver the completion packet
}

void
win_socket_impl_internal::
do_write_io()
{
    auto& op = wr_;

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
            // Immediate error - must use post(). See do_read_io for explanation.
            svc_.work_finished();
            op.dwError = err;
            svc_.post(&op);
            return;
        }
    }
    // Synchronous completion: IOCP will deliver the completion packet
}

//------------------------------------------------------------------------------

std::coroutine_handle<>
win_socket_impl_internal::
read_some(
    capy::coro h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    rd_.internal_ptr = shared_from_this();

    auto& op = rd_;
    op.reset();
    op.is_read_ = true;
    op.h = h;
    op.ex = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Prepare buffers (must happen before initiator runs)
    capy::mutable_buffer bufs[read_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, read_op::max_buffers));

    // Handle empty buffer: complete with 0 bytes via post for consistency
    if (op.wsabuf_count == 0)
    {
        op.bytes_transferred = 0;
        op.dwError = 0;
        op.empty_buffer = true;
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    // Symmetric transfer to initiator - I/O starts after caller is suspended
    return read_initiator_.start<&win_socket_impl_internal::do_read_io>(this);
}

std::coroutine_handle<>
win_socket_impl_internal::
write_some(
    capy::coro h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    wr_.internal_ptr = shared_from_this();

    auto& op = wr_;
    op.reset();
    op.h = h;
    op.ex = d;
    op.ec_out = ec;
    op.bytes_out = bytes_out;
    op.start(token);

    // Prepare buffers (must happen before initiator runs)
    capy::mutable_buffer bufs[write_op::max_buffers];
    op.wsabuf_count = static_cast<DWORD>(
        param.copy_to(bufs, write_op::max_buffers));

    // Handle empty buffer: complete immediately with 0 bytes
    if (op.wsabuf_count == 0)
    {
        op.bytes_transferred = 0;
        op.dwError = 0;
        svc_.post(&op);
        return std::noop_coroutine();
    }

    for (DWORD i = 0; i < op.wsabuf_count; ++i)
    {
        op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
        op.wsabufs[i].len = static_cast<ULONG>(bufs[i].size());
    }

    // Symmetric transfer to initiator - I/O starts after caller is suspended
    return write_initiator_.start<&win_socket_impl_internal::do_write_io>(this);
}

void
win_socket_impl_internal::
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
win_socket_impl_internal::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    // Clear cached endpoints
    local_endpoint_ = endpoint{};
    remote_endpoint_ = endpoint{};
}

void
win_socket_impl::
release()
{
    if (internal_)
    {
        auto& svc = internal_->svc_;
        internal_->release_internal();
        internal_.reset();
        svc.destroy_impl(*this);
    }
}

win_sockets::
win_sockets(
    capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
    load_extension_functions();
}

win_sockets::
~win_sockets()
{
}

void
win_sockets::
shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    // Just close sockets and remove from list
    // The shared_ptrs held by socket objects and operations will handle destruction
    for (auto* impl = socket_list_.pop_front(); impl != nullptr;
         impl = socket_list_.pop_front())
    {
        impl->close_socket();
        // Note: impl may still be alive if operations hold shared_ptr
    }

    for (auto* impl = acceptor_list_.pop_front(); impl != nullptr;
         impl = acceptor_list_.pop_front())
    {
        impl->close_socket();
    }

    // Cleanup wrappers
    for (auto* w = socket_wrapper_list_.pop_front(); w != nullptr;
         w = socket_wrapper_list_.pop_front())
    {
        delete w;
    }

    for (auto* w = acceptor_wrapper_list_.pop_front(); w != nullptr;
         w = acceptor_wrapper_list_.pop_front())
    {
        delete w;
    }
}

win_socket_impl&
win_sockets::
create_impl()
{
    auto internal = std::make_shared<win_socket_impl_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_list_.push_back(internal.get());
    }

    auto* wrapper = new win_socket_impl(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.push_back(wrapper);
    }

    return *wrapper;
}

void
win_sockets::
destroy_impl(win_socket_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        socket_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

void
win_sockets::
unregister_impl(win_socket_impl_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    socket_list_.remove(&impl);
}

std::error_code
win_sockets::
open_socket(win_socket_impl_internal& impl)
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
        return make_err(::WSAGetLastError());

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        key_io,
        0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    impl.socket_ = sock;
    return {};
}

void
win_sockets::
post(overlapped_op* op)
{
    sched_.post(op);
}

void
win_sockets::
work_started() noexcept
{
    sched_.work_started();
}

void
win_sockets::
work_finished() noexcept
{
    sched_.work_finished();
}

void
win_sockets::
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
win_sockets::
create_acceptor_impl()
{
    auto internal = std::make_shared<win_acceptor_impl_internal>(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_list_.push_back(internal.get());
    }

    auto* wrapper = new win_acceptor_impl(std::move(internal));

    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_wrapper_list_.push_back(wrapper);
    }

    return *wrapper;
}

void
win_sockets::
destroy_acceptor_impl(win_acceptor_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        acceptor_wrapper_list_.remove(&impl);
    }
    delete &impl;
}

void
win_sockets::
unregister_acceptor_impl(win_acceptor_impl_internal& impl)
{
    std::lock_guard<win_mutex> lock(mutex_);
    acceptor_list_.remove(&impl);
}

std::error_code
win_sockets::
open_acceptor(
    win_acceptor_impl_internal& impl,
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
        return make_err(::WSAGetLastError());

    // Allow address reuse
    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<char*>(&reuse), sizeof(reuse));

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock),
        static_cast<HANDLE>(iocp_),
        key_io,
        0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    // Bind to endpoint
    sockaddr_in addr = detail::to_sockaddr_in(ep);
    if (::bind(sock,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)) == SOCKET_ERROR)
    {
        DWORD dwError = ::WSAGetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    // Start listening
    if (::listen(sock, backlog) == SOCKET_ERROR)
    {
        DWORD dwError = ::WSAGetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    impl.socket_ = sock;

    // Cache the local endpoint (queries OS for ephemeral port if port was 0)
    sockaddr_in local_addr{};
    int local_len = sizeof(local_addr);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
        impl.set_local_endpoint(detail::from_sockaddr_in(local_addr));

    return {};
}

win_acceptor_impl_internal::
win_acceptor_impl_internal(win_sockets& svc) noexcept
    : svc_(svc)
{
}

win_acceptor_impl_internal::
~win_acceptor_impl_internal()
{
    svc_.unregister_acceptor_impl(*this);
}

void
win_acceptor_impl_internal::
release_internal()
{
    // Cancel pending I/O before closing to ensure operations
    // complete with ERROR_OPERATION_ABORTED via IOCP
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(socket_),
            nullptr);
    }
    close_socket();
    // Destruction happens automatically when all shared_ptrs are released
}

std::coroutine_handle<>
win_acceptor_impl_internal::
accept(
    capy::coro h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    io_object::io_object_impl** impl_out)
{
    // Keep acceptor internal alive during I/O
    acc_.acceptor_ptr = shared_from_this();

    auto& op = acc_;
    op.reset();
    op.h = h;
    op.ex = d;
    op.ec_out = ec;
    op.impl_out = impl_out;
    op.start(token);

    // Create wrapper for the peer socket (service owns it)
    auto& peer_wrapper = svc_.create_impl();

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
        peer_wrapper.release();
        op.dwError = ::WSAGetLastError();
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(accepted),
        svc_.native_handle(),
        key_io,
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(accepted);
        peer_wrapper.release();
        op.dwError = err;
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    // Set up the accept operation
    op.accepted_socket = accepted;
    op.peer_wrapper = &peer_wrapper;
    op.listen_socket = socket_;

    auto accept_ex = svc_.accept_ex();
    if (!accept_ex)
    {
        ::closesocket(accepted);
        peer_wrapper.release();
        op.peer_wrapper = nullptr;
        op.accepted_socket = INVALID_SOCKET;
        op.dwError = WSAEOPNOTSUPP;
        svc_.post(&op);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
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
            peer_wrapper.release();
            op.peer_wrapper = nullptr;
            op.accepted_socket = INVALID_SOCKET;
            op.dwError = err;
            svc_.post(&op);
            // completion is always posted to scheduler queue, never inline.
            return std::noop_coroutine();
        }
    }
    // Synchronous completion: IOCP will deliver the completion packet
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

void
win_acceptor_impl_internal::
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
win_acceptor_impl_internal::
close_socket() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    // Clear cached endpoint
    local_endpoint_ = endpoint{};
}

void
win_acceptor_impl::
release()
{
    if (internal_)
    {
        auto& svc = internal_->svc_;
        internal_->release_internal();
        internal_.reset();
        svc.destroy_acceptor_impl(*this);
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP
