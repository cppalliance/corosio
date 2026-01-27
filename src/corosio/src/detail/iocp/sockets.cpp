//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/sockets.hpp"
#include "src/detail/iocp/scheduler.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

/*
    Windows IOCP Socket Implementation Overview
    ===========================================

    This file implements asynchronous socket I/O using Windows I/O Completion
    Ports (IOCP). Understanding the following concepts is essential for
    maintaining this code.

    IOCP Fundamentals
    -----------------
    IOCP is a kernel-managed queue for I/O completions. The flow is:

    1. Associate a socket with the IOCP via CreateIoCompletionPort()
    2. Start async I/O (WSARecv, WSASend, ConnectEx, AcceptEx) passing an
       OVERLAPPED structure
    3. The kernel performs the I/O asynchronously
    4. When complete, the kernel posts a completion packet to the IOCP
    5. GetQueuedCompletionStatus() dequeues completions for processing

    Our overlapped_op derives from OVERLAPPED, so we can static_cast between
    them. Each operation type (connect_op, read_op, write_op, accept_op)
    contains all state needed for that I/O operation.

    Completion Key Dispatch
    -----------------------
    Each socket is associated with a completion_key pointer when registered
    with the IOCP. When a completion arrives, we dispatch through the key's
    virtual on_completion() method. The overlapped_key handles socket I/O
    completions by:

    1. Casting the OVERLAPPED* back to overlapped_op*
    2. Using InterlockedCompareExchange on ready_ to handle races
    3. Calling complete() to store results, then operator() to resume

    The ready_ flag handles a subtle race: an operation can complete
    synchronously (returning immediately) but IOCP still posts a completion.
    The first path to set ready_=1 wins and processes the completion.

    Lifetime Management via shared_ptr (Hidden from Public Interface)
    -----------------------------------------------------------------
    The trickiest aspect is ensuring socket state stays alive while I/O is
    pending. Consider: socket::close() is called while a read is in flight.
    We must:

    1. Cancel the I/O (CancelIoEx)
    2. Close the socket handle (closesocket)
    3. But the internal state CANNOT be destroyed yet - IOCP will still
       deliver a completion packet for the cancelled I/O

    We use a two-layer design to hide shared_ptr from the public interface:

    1. win_socket_impl (wrapper) - what the socket class sees
       - Derives from socket::socket_impl
       - Holds shared_ptr<win_socket_impl_internal>
       - Owned by win_sockets service (tracked via intrusive_list)
       - Destroyed by release() which calls svc_.destroy_impl()

    2. win_socket_impl_internal - actual state + operations
       - Derives from enable_shared_from_this
       - Contains socket handle, connect_op, read_op, write_op
       - May outlive the wrapper if operations are pending

    When I/O starts, operations capture shared_from_this() on the internal:
       conn_.internal_ptr = shared_from_this()

    When socket::close() is called:
    1. wrapper->release() cancels I/O and closes socket handle
    2. release() calls svc_.destroy_impl() which deletes the wrapper
    3. Internal may still be alive if operations hold refs
    4. When operation completes, internal_ptr.reset() releases the ref
    5. If that was the last ref, internal is destroyed

    Key Invariants
    --------------
    1. Operations hold shared_ptr<internal> ONLY during active I/O (set at
       I/O start, cleared in operator())

    2. The win_sockets service owns both wrappers and tracks internals:
       - socket_wrapper_list_ / acceptor_wrapper_list_ own wrappers
       - socket_list_ / acceptor_list_ track internals for shutdown

    3. Internal impl destructors call unregister_impl() to remove themselves
       from the service's list

    4. The socket/acceptor classes hold raw pointers to wrappers; wrappers
       hold shared_ptr to internals. No shared_ptr in public headers.

    5. For accept operations, a new wrapper is created by the service and
       passed to the peer socket via impl_out. The peer socket calls
       release() on close, which triggers destroy_impl().

    Cancellation
    ------------
    Cancellation has two paths:

    1. Explicit cancel(): Sets the cancelled flag and calls CancelIoEx().
       The completion will arrive with ERROR_OPERATION_ABORTED.

    2. Stop token: The stop_callback calls request_cancel() which does the
       same thing. The stop_cb is reset in operator() before resuming.

    Both paths result in the operation completing normally through IOCP,
    just with an error code. The coroutine resumes and sees the cancellation.

    Service Shutdown
    ----------------
    When the io_context shuts down, win_sockets::shutdown() closes all
    sockets and removes them from the tracking list, then deletes any
    remaining wrappers. Internals may still be alive if operations hold
    shared_ptrs. This is fine - they'll be destroyed when all references
    are released.

    Thread Safety
    -------------
    - Multiple threads can call GetQueuedCompletionStatus() on the same IOCP
    - The mutex_ protects the socket/acceptor lists during create/unregister
    - Individual socket operations are NOT thread-safe - users must not
      have concurrent operations of the same type on a single socket
*/

namespace boost::corosio::detail {

completion_key::result
win_sockets::overlapped_key::
on_completion(
    win_scheduler& sched,
    DWORD bytes,
    DWORD dwError,
    LPOVERLAPPED overlapped)
{
    auto* op = static_cast<overlapped_op*>(overlapped);
    if (::InterlockedCompareExchange(&op->ready_, 1, 0) == 0)
    {
        struct work_guard
        {
            win_scheduler* self;
            ~work_guard() { self->on_work_finished(); }
        };

        work_guard g{&sched};
        op->complete(bytes, dwError);
        (*op)();
        return result::did_work;
    }
    return result::continue_loop;
}

void
win_sockets::overlapped_key::
destroy(LPOVERLAPPED overlapped)
{
    static_cast<overlapped_op*>(overlapped)->destroy();
}

void
accept_op::
operator()()
{
    stop_cb.reset();

    bool success = (dwError == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (dwError != 0)
            *ec_out = make_err(dwError);
    }

    if (success && accepted_socket != INVALID_SOCKET && peer_wrapper)
    {
        // Update accept context for proper socket behavior
        ::setsockopt(
            accepted_socket,
            SOL_SOCKET,
            SO_UPDATE_ACCEPT_CONTEXT,
            reinterpret_cast<char*>(&listen_socket),
            sizeof(SOCKET));

        // Transfer socket handle to peer impl internal
        peer_wrapper->get_internal()->set_socket(accepted_socket);

        // Cache endpoints on the accepted socket
        sockaddr_in local_addr{};
        int local_len = sizeof(local_addr);
        sockaddr_in remote_addr{};
        int remote_len = sizeof(remote_addr);

        endpoint local_ep, remote_ep;
        if (::getsockname(accepted_socket,
            reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        if (::getpeername(accepted_socket,
            reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0)
            remote_ep = from_sockaddr_in(remote_addr);

        peer_wrapper->get_internal()->set_endpoints(local_ep, remote_ep);

        accepted_socket = INVALID_SOCKET;

        // Pass wrapper to awaitable for assignment to peer socket
        if (impl_out)
            *impl_out = peer_wrapper;
        // Note: peer_wrapper ownership transfers to the peer socket
        // Don't delete it here
    }
    else
    {
        // Cleanup on failure
        if (accepted_socket != INVALID_SOCKET)
        {
            ::closesocket(accepted_socket);
            accepted_socket = INVALID_SOCKET;
        }

        // Release the peer wrapper on failure
        peer_wrapper->release();
        peer_wrapper = nullptr;

        if (impl_out)
            *impl_out = nullptr;
    }

    // Save h and d before resetting acceptor_ptr, because acceptor_ptr
    // may be the last reference to the internal, and this accept_op is a
    // member of the internal. Destroying the internal would invalidate h and d.
    auto saved_h = h;
    auto saved_d = d;

    // Release the acceptor reference now that I/O is complete
    acceptor_ptr.reset();

    saved_d.dispatch(saved_h).resume();
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
connect_op::
operator()()
{
    // Cache endpoints on successful connect
    bool success = (dwError == 0 && !cancelled.load(std::memory_order_acquire));
    if (success && internal.is_open())
    {
        // Query local endpoint via getsockname (may fail, but remote is always known)
        endpoint local_ep;
        sockaddr_in local_addr{};
        int local_len = sizeof(local_addr);
        if (::getsockname(internal.native_handle(),
            reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
            local_ep = from_sockaddr_in(local_addr);
        // Always cache remote endpoint; local may be default if getsockname failed
        internal.set_endpoints(local_ep, target_endpoint);
    }

    overlapped_op::operator()();
    internal_ptr.reset();
}

void
connect_op::
do_cancel() noexcept
{
    if (internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(internal.native_handle()),
            this);
    }
}

void
read_op::
operator()()
{
    overlapped_op::operator()();
    internal_ptr.reset();
}

void
read_op::
do_cancel() noexcept
{
    if (internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(internal.native_handle()),
            this);
    }
}

void
write_op::
operator()()
{
    overlapped_op::operator()();
    internal_ptr.reset();
}

void
write_op::
do_cancel() noexcept
{
    if (internal.is_open())
    {
        ::CancelIoEx(
            reinterpret_cast<HANDLE>(internal.native_handle()),
            this);
    }
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
    // Destruction happens automatically when all shared_ptrs are released
}

void
win_socket_impl_internal::
connect(
    capy::coro h,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    system::error_code* ec)
{
    // Keep internal alive during I/O
    conn_.internal_ptr = shared_from_this();

    auto& op = conn_;
    op.reset();
    op.h = h;
    op.d = d;
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
        return;
    }

    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        op.dwError = WSAEOPNOTSUPP;
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
            op.dwError = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        // Synchronous completion - with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS,
        // IOCP shouldn't post a packet. But if the flag failed to set or under
        // certain conditions, IOCP might still deliver a completion. Use CAS
        // to race with IOCP: only set fields and post if we win (CAS returns 0).
        // If IOCP wins, it already set the fields via complete() and processed.
        svc_.work_finished();
        if (::InterlockedCompareExchange(&op.ready_, 1, 0) == 0)
        {
            op.dwError = 0;
            svc_.post(&op);
        }
    }
}

void
win_socket_impl_internal::
read_some(
    capy::coro h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    rd_.internal_ptr = shared_from_this();

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

    // Handle empty buffer: complete immediately with 0 bytes
    if (op.wsabuf_count == 0)
    {
        op.bytes_transferred = 0;
        op.dwError = 0;
        op.empty_buffer = true;
        svc_.post(&op);
        return;
    }

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
            op.dwError = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        // Synchronous completion - with FILE_SKIP_COMPLETION_PORT_ON_SUCCESS,
        // IOCP shouldn't post a packet. But if the flag failed to set or under
        // certain conditions, IOCP might still deliver a completion. Use CAS
        // to race with IOCP: only set fields and post if we win (CAS returns 0).
        // If IOCP wins, it already set the fields via complete() and processed.
        svc_.work_finished();
        if (::InterlockedCompareExchange(&op.ready_, 1, 0) == 0)
        {
            op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
            op.dwError = 0;
            svc_.post(&op);
        }
    }
}

void
win_socket_impl_internal::
write_some(
    capy::coro h,
    capy::executor_ref d,
    io_buffer_param param,
    std::stop_token token,
    system::error_code* ec,
    std::size_t* bytes_out)
{
    // Keep internal alive during I/O
    wr_.internal_ptr = shared_from_this();

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

    // Handle empty buffer: complete immediately with 0 bytes
    if (op.wsabuf_count == 0)
    {
        op.bytes_transferred = 0;
        op.dwError = 0;
        svc_.post(&op);
        return;
    }

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
            op.dwError = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        // Synchronous completion - use CAS to race with IOCP.
        // See read_some for detailed explanation.
        svc_.work_finished();
        if (::InterlockedCompareExchange(&op.ready_, 1, 0) == 0)
        {
            op.bytes_transferred = static_cast<DWORD>(op.InternalHigh);
            op.dwError = 0;
            svc_.post(&op);
        }
    }
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

system::error_code
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
        reinterpret_cast<ULONG_PTR>(&overlapped_key_),
        0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
    }

    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(sock),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

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

system::error_code
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
        reinterpret_cast<ULONG_PTR>(&overlapped_key_),
        0);

    if (result == nullptr)
    {
        DWORD dwError = ::GetLastError();
        ::closesocket(sock);
        return make_err(dwError);
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

void
win_acceptor_impl_internal::
accept(
    capy::coro h,
    capy::executor_ref d,
    std::stop_token token,
    system::error_code* ec,
    io_object::io_object_impl** impl_out)
{
    // Keep acceptor internal alive during I/O
    acc_.acceptor_ptr = shared_from_this();

    auto& op = acc_;
    op.reset();
    op.h = h;
    op.d = d;
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
        return;
    }

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(accepted),
        svc_.native_handle(),
        reinterpret_cast<ULONG_PTR>(svc_.io_key()),
        0);

    if (result == nullptr)
    {
        DWORD err = ::GetLastError();
        ::closesocket(accepted);
        peer_wrapper.release();
        op.dwError = err;
        svc_.post(&op);
        return;
    }

    ::SetFileCompletionNotificationModes(
        reinterpret_cast<HANDLE>(accepted),
        FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

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
            peer_wrapper.release();
            op.peer_wrapper = nullptr;
            op.accepted_socket = INVALID_SOCKET;
            op.dwError = err;
            svc_.post(&op);
            return;
        }
    }
    else
    {
        // Synchronous completion - use CAS to race with IOCP.
        // See win_socket_impl_internal::read_some for detailed explanation.
        svc_.work_finished();
        if (::InterlockedCompareExchange(&op.ready_, 1, 0) == 0)
        {
            op.dwError = 0;
            svc_.post(&op);
        }
    }
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

#endif // _WIN32
