//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/iocp/win_dissociate.hpp>
#include <boost/corosio/native/detail/iocp/win_validate_handle.hpp>
#include <boost/corosio/native/detail/iocp/win_wsa_init.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>

#include <boost/corosio/native/detail/iocp/win_tcp_socket.hpp>
#include <boost/corosio/native/detail/iocp/win_tcp_acceptor.hpp>

#include <mswsock.h>

namespace boost::corosio::detail {

class win_scheduler;
class win_tcp_acceptor;
class win_tcp_acceptor_service;

/** Return whether the library puts sockets in skip-on-success mode.

    False when the library itself was built with
    `BOOST_COROSIO_IOCP_NO_SKIP_ON_SUCCESS` defined, which leaves every
    socket out of the mode: that is how the tests are run over the path
    a socket takes when the mode cannot be set. The macro is read only
    where the library is compiled; defining it for any other
    translation unit changes nothing, so no two of them can disagree.
*/
BOOST_COROSIO_DECL bool iocp_skip_on_success_enabled() noexcept;

/** Windows IOCP socket management service.

    This service owns all socket and acceptor implementations and
    coordinates their lifecycle with the IOCP. It provides:

    - Socket/acceptor implementation recycling through `object_pool`
    - IOCP handle association for sockets
    - Function pointer loading for ConnectEx/AcceptEx
    - Graceful shutdown - closes every live implementation when the
      io_context stops; pool destruction frees whatever is left

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class BOOST_COROSIO_DECL win_tcp_service final
    : private win_wsa_init
    , public capy::execution_context::service
    , public io_object::io_service
{
    friend class win_tcp_socket;
    friend class win_tcp_acceptor;

public:
    using key_type = win_tcp_service;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override;

    void close(io_object::handle& h) override;

    /** Construct the socket service.

        Obtains the IOCP handle from the scheduler service and
        loads extension function pointers.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_tcp_service(capy::execution_context& ctx);

    win_tcp_service(win_tcp_service const&)            = delete;
    win_tcp_service& operator=(win_tcp_service const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create and register a socket with the IOCP.

        @param impl The socket implementation to initialize.
        @return Error code, or success.
    */
    std::error_code open_socket(
        win_tcp_socket& impl, int family, int type, int protocol);

    /** Adopt an existing socket handle into an implementation.

        Validates family and type before touching the held socket,
        then associates the new socket with the IOCP. On success the
        impl takes ownership and will close the handle; on failure
        the caller retains ownership.

        @param impl The socket implementation to assign to.
        @param fd The native socket handle to adopt.
        @return Error code, or success.
    */
    std::error_code assign_socket(win_tcp_socket& impl, native_handle_type fd);

    /** Bind a stream socket to a local endpoint.

        @param impl The socket implementation to bind.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code bind_socket(win_tcp_socket& impl, endpoint ep);

    /** Pop a recycled acceptor or news one, with the service reference
        held. Used by `win_tcp_acceptor_service::construct()`.
    */
    win_tcp_acceptor* acquire_acceptor_impl();

    /** Create an acceptor socket without binding or listening.

        Creates a socket and associates it with the IOCP.
        For IPv6, dual-stack is enabled by default.
        Does not set SO_REUSEADDR.

        @param impl The acceptor implementation to initialize.
        @param family Address family (e.g. `AF_INET`, `AF_INET6`).
        @param type Socket type (e.g. `SOCK_STREAM`).
        @param protocol Protocol number (e.g. `IPPROTO_TCP`).
        @return Error code, or success.
    */
    std::error_code open_acceptor_socket(
        win_tcp_acceptor& impl, int family, int type, int protocol);

    /** Adopt an existing listening socket into an acceptor.

        Validates the socket, associates it with the IOCP, and only
        then releases the socket the acceptor already held. Listen
        state is not verified.

        @param impl The acceptor implementation.
        @param fd The native socket to adopt. Ownership transfers only
            on success.
        @return Error code, or success.
    */
    std::error_code
    assign_acceptor_socket(win_tcp_acceptor& impl, native_handle_type fd);

    /** Bind an open acceptor to a local endpoint.

        @param impl The acceptor implementation.
        @param ep The local endpoint to bind to.
        @return Error code, or success.
    */
    std::error_code bind_acceptor(win_tcp_acceptor& impl, endpoint ep);

    /** Start listening for incoming connections.

        @param impl The acceptor implementation.
        @param backlog The listen backlog.
        @return Error code, or success.
    */
    std::error_code listen_acceptor(win_tcp_acceptor& impl, int backlog);

    /** Return the IOCP handle. */
    void* native_handle() const noexcept;

    /** Return the ConnectEx function pointer. */
    LPFN_CONNECTEX connect_ex() const noexcept
    {
        return connect_ex_;
    }

    /** Return the AcceptEx function pointer. */
    LPFN_ACCEPTEX accept_ex() const noexcept
    {
        return accept_ex_;
    }

    /** Return the GetAcceptExSockaddrs function pointer, or null. */
    LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs() const noexcept
    {
        return get_accept_ex_sockaddrs_;
    }

    /** Post an overlapped operation for completion. */
    void post(overlapped_op* op);

    /** Signal that an overlapped I/O is now pending (CAS protocol). */
    void on_pending(overlapped_op* op) noexcept;

    /** Post an immediate completion with pre-stored results. */
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) noexcept;

    /** Notify scheduler of pending I/O work. */
    void work_started() noexcept;

    /** Notify scheduler that I/O work completed. */
    void work_finished() noexcept;

    /** Return the owning IOCP scheduler. */
    win_scheduler& scheduler() noexcept
    {
        return sched_;
    }

    /** Put a socket just bound to the port in skip-on-success mode.

        In that mode a request the kernel finishes during the call
        queues no completion packet, and the caller takes the result
        from the call. The mode cannot be cleared again. It is only
        safe when the socket's provider hands out real kernel handles,
        which is checked once per address family.

        @return True if the mode is now set on the socket.
    */
    bool enable_skip_on_success(SOCKET s, int family) const noexcept;

private:
    void load_extension_functions();

    // Whether TCP sockets of the family are installable-file-system
    // handles: kernel handles, which skip-on-success mode needs and
    // whose pending requests are marked in the OVERLAPPED.
    static bool provider_is_ifs(int family) noexcept;

    win_scheduler& sched_;
    void* iocp_;
    bool ifs_v4_               = false;
    bool ifs_v6_               = false;
    LPFN_CONNECTEX connect_ex_ = nullptr;
    LPFN_ACCEPTEX accept_ex_   = nullptr;
    // Optional: an accept parses its endpoints out of the AcceptEx
    // buffer with it, and asks the kernel for them without it.
    LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs_ = nullptr;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_tcp_socket> pool_;
    object_pool<win_tcp_acceptor> acceptor_pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

// Operation constructors

inline connect_op::connect_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline read_op::read_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline write_op::write_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

inline wait_op::wait_op(win_tcp_socket& internal_) noexcept
    : overlapped_op(&do_complete)
    , internal(internal_)
{
    cancel_func_ = &do_cancel_impl;
}

// Cancellation functions

inline void
connect_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<connect_op*>(base);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
read_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<read_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
write_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<write_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
}

inline void
wait_op::do_cancel_impl(overlapped_op* base) noexcept
{
    auto* op = static_cast<wait_op*>(base);
    op->cancelled.store(true, std::memory_order_release);
    // Best-effort cancel of any pending zero-byte WSARecv issued for
    // wait_type::read. ERROR_NOT_FOUND when nothing overlapped is
    // pending is harmless.
    if (op->internal.socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(op->internal.socket_), op);
    }
    // wait_type::error parks the op in the auxiliary select reactor;
    // wake it so the reactor can post a cancelled completion. A cancel
    // for an op the reactor never registered finds nothing and returns.
    op->internal.svc_.scheduler().cancel_wait(op);
}

// connect_op completion handler

inline void
connect_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<connect_op*>(base);

    if (!owner)
    {
        op->destroy_awaiting();
        return;
    }

    bool success =
        (op->dwError == 0 && !op->cancelled.load(std::memory_order_acquire));
    if (success && op->internal.socket_ != INVALID_SOCKET)
    {
        // Required after ConnectEx to enable shutdown(), getsockname(), etc.
        ::setsockopt(
            op->internal.socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
            nullptr, 0);

        // Most connections never read their local endpoint, so it is
        // left for local_endpoint() to fetch rather than costing every
        // connect a getsockname.
        op->internal.remote_endpoint_ = op->target_endpoint;
        op->internal.local_state_.store(
            win_tcp_socket::local_state::pending, std::memory_order_release);
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// read_op completion handler

inline void
read_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<read_op*>(base);

    if (!owner)
    {
        op->destroy_awaiting();
        return;
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// write_op completion handler

inline void
write_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<write_op*>(base);

    if (!owner)
    {
        op->destroy_awaiting();
        return;
    }

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// wait_op completion handler

inline void
wait_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<wait_op*>(base);

    if (!owner)
    {
        op->destroy_awaiting();
        return;
    }

    // Only the zero-byte WSARecv read wait; write and error waits come
    // from the poll reactor through this op too.
    if (op->w == wait_type::read)
        op->dwError = normalize_read_wait_error(op->dwError);

    auto prevent_premature_destruction = std::move(op->object_ref_);
    op->invoke_handler();
}

// win_tcp_socket

inline win_tcp_socket::win_tcp_socket(win_tcp_service& svc) noexcept
    : svc_(svc)
    , conn_(*this)
    , rd_(*this)
    , wr_(*this)
    , wt_(*this)
{
}

inline void
win_tcp_socket::retire() noexcept
{
    svc_.pool_.recycle(this);
}

inline void
win_tcp_socket::reuse() noexcept
{
    BOOST_COROSIO_ASSERT(socket_ == INVALID_SOCKET);
    BOOST_COROSIO_ASSERT(family_ == AF_UNSPEC);
    // A connect completing on another thread while the socket closed
    // can still have written these.
    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
    local_state_.store(local_state::cached, std::memory_order_relaxed);
    BOOST_COROSIO_ASSERT(!conn_.stop_cb);
    BOOST_COROSIO_ASSERT(!rd_.stop_cb);
    BOOST_COROSIO_ASSERT(!wr_.stop_cb);
    BOOST_COROSIO_ASSERT(!wt_.stop_cb);
}

inline void
win_tcp_socket::set_socket(SOCKET s, bool skip, bool ifs) noexcept
{
    socket_ = s;
    skip_   = skip;
    ifs_    = ifs;
}

inline void
win_tcp_socket::set_endpoints(endpoint local, endpoint remote) noexcept
{
    local_endpoint_  = local;
    remote_endpoint_ = remote;
    local_state_.store(local_state::cached, std::memory_order_release);
}

inline std::coroutine_handle<>
win_tcp_socket::connect(
    capy::continuation& cont,
    capy::executor_ref d,
    endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    // Keep this socket alive during I/O
    conn_.object_ref_ = detail::object_ref(this);

    auto& op = conn_;
    op.reset();
    op.cont            = &cont;
    op.ex              = d;
    op.ec_out          = ec;
    op.target_endpoint = ep;
    op.start(token);

    svc_.work_started();

    // ConnectEx requires the socket to be bound. A cache that names an
    // endpoint says it is (e.g. tcp_socket::bind() before connect). An
    // endpoint an earlier connect left unfetched says it most likely
    // is, but that mark can outlive the socket it described when a late
    // completion races a close and reopen, so the bind is tried anyway
    // and an "already bound" refusal leaves the verdict to ConnectEx.
    bool const unfetched =
        local_state_.load(std::memory_order_acquire) != local_state::cached;
    if (unfetched || local_endpoint_ == endpoint{})
    {
        sockaddr_storage bind_storage{};
        socklen_t bind_len;
        if (family_ == AF_INET6)
        {
            sockaddr_in6 sa6{};
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port   = 0;
            sa6.sin6_addr   = in6addr_any;
            std::memcpy(&bind_storage, &sa6, sizeof(sa6));
            bind_len = sizeof(sa6);
        }
        else
        {
            sockaddr_in sa4{};
            sa4.sin_family      = AF_INET;
            sa4.sin_addr.s_addr = INADDR_ANY;
            sa4.sin_port        = 0;
            std::memcpy(&bind_storage, &sa4, sizeof(sa4));
            bind_len = sizeof(sa4);
        }

        if (::bind(
                socket_, reinterpret_cast<sockaddr*>(&bind_storage),
                bind_len) == SOCKET_ERROR)
        {
            int const err = ::WSAGetLastError();
            if (!unfetched || err != WSAEINVAL)
            {
                svc_.on_completion(&op, static_cast<DWORD>(err), 0);
                return std::noop_coroutine();
            }
        }
    }

    auto connect_ex = svc_.connect_ex();
    if (!connect_ex)
    {
        svc_.on_completion(&op, WSAEOPNOTSUPP, 0);
        return std::noop_coroutine();
    }

    sockaddr_storage storage{};
    socklen_t addrlen = detail::to_sockaddr(ep, family_, storage);

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
    else if (skip_)
    {
        // Connected during the call, and this socket queues no packet
        // for that.
        svc_.on_completion(&op, 0, 0);
        return std::noop_coroutine();
    }

    // A stop before ConnectEx was issued found nothing to cancel. Must
    // precede on_pending: once it runs another thread may complete the
    // op and recycle this impl.
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    svc_.on_pending(&op);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::read_some(
    capy::continuation& cont,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();
    op.is_read = true;

    // What the call itself settled. `queued` says a packet will carry
    // the outcome instead.
    DWORD err   = 0;
    DWORD bytes = 0;
    bool queued = false;

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel.
    if (socket_ == INVALID_SOCKET)
    {
        err = WSAEBADF;
    }
    else
    {
        // Prepare buffers
        capy::mutable_buffer bufs[read_op::max_buffers];
        op.wsabuf_count =
            static_cast<DWORD>(param.copy_to(bufs, read_op::max_buffers));

        if (op.wsabuf_count == 0)
        {
            // Empty buffer: complete with 0 bytes
            op.empty_buffer = true;
        }
        else
        {
            for (DWORD i = 0; i < op.wsabuf_count; ++i)
            {
                op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
                op.wsabufs[i].len = static_cast<ULONG>(
                    (std::min)(bufs[i].size(), std::size_t(0x7fffffff)));
            }

            op.flags = 0;

            queued = queued_after(
                ::WSARecv(
                    socket_, op.wsabufs, op.wsabuf_count, &bytes, &op.flags,
                    &op, nullptr),
                err);
        }
    }

    if (!queued)
        return complete_now(op, cont, d, token, ec, bytes_out, err, bytes);

    return pend(op, cont, d, token, ec, bytes_out);
}

inline void
win_tcp_socket::fill(
    overlapped_op& op,
    capy::continuation& cont,
    capy::executor_ref d,
    std::error_code* ec,
    std::size_t* bytes_out) noexcept
{
    op.object_ref_ = detail::object_ref(this);
    op.cont        = &cont;
    op.ex          = d;
    op.ec_out      = ec;
    op.bytes_out   = bytes_out;
}

inline bool
win_tcp_socket::queued_after(int rc, DWORD& err) const noexcept
{
    // An immediate success still queues its packet unless the socket
    // skips it.
    if (rc != SOCKET_ERROR)
        return !skip_;
    err = ::WSAGetLastError();
    return err == WSA_IO_PENDING;
}

inline std::coroutine_handle<>
win_tcp_socket::complete_now(
    overlapped_op& op,
    capy::continuation& cont,
    capy::executor_ref d,
    std::stop_token const& token,
    std::error_code* ec,
    std::size_t* bytes_out,
    DWORD err,
    DWORD bytes)
{
    // A transfer is reported as it is. With nothing transferred a
    // stop or cancel that was already requested names the outcome, as
    // it does for an operation that went through the port.
    bool const cancelled = bytes == 0 &&
        (op.cancelled.load(std::memory_order_acquire) ||
         token.stop_requested());

    // A large transfer goes through the loop, behind whatever the
    // port already holds, which lets the peer's side run before the
    // next one: left to run on, a writer piles far more into the
    // kernel's buffers than the cache holds, and the reader then
    // takes it all cold.
    constexpr DWORD inline_transfer_max = 64 * 1024;
    bool const large                    = bytes > inline_transfer_max;

    auto& sched = svc_.scheduler();
    if (!large && sched.try_consume_inline_budget())
    {
        // The kernel is done with the request and no packet refers to
        // the op, so the coroutine carries on from here. Nothing was
        // armed and no work was counted.
        decode_results(
            ec, bytes_out, cancelled,
            err != 0 ? iocp_make_err(err, /*accept_path=*/false)
                     : std::error_code{},
            op.is_read, static_cast<std::size_t>(bytes), op.empty_buffer);
        return dispatch_coro(d, cont);
    }

    // Off the loop, or the handler has used its inline budget: the
    // result goes through the run loop so other work gets its turn.
    fill(op, cont, d, ec, bytes_out);
    if (cancelled)
        op.request_cancel();
    // A stop that arrives before the loop gets to the result still
    // names the outcome of an operation that transferred nothing.
    op.arm_stop(token);
    svc_.work_started();
    if (large)
        sched.request_port_visit();
    svc_.on_completion(&op, err, bytes);
    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::pend(
    overlapped_op& op,
    capy::continuation& cont,
    capy::executor_ref d,
    std::stop_token const& token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    // The request is in the kernel and its packet is owed. No thread
    // dispatches the op before on_pending below, so it is filled in
    // here, after the call, where an operation that completed inline
    // never pays for it.
    fill(op, cont, d, ec, bytes_out);

    // A stop already requested runs the callback here, which cancels
    // the request just issued.
    op.arm_stop(token);

    svc_.work_started();

    // A cancel that raced the call found nothing to cancel. Must
    // precede on_pending: once it runs another thread may complete
    // the op and recycle this impl.
    if (op.cancelled.load(std::memory_order_acquire))
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

    svc_.on_pending(&op);

    return std::noop_coroutine();
}

inline std::coroutine_handle<>
win_tcp_socket::write_some(
    capy::continuation& cont,
    capy::executor_ref d,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    // As in read_some.
    DWORD err   = 0;
    DWORD bytes = 0;
    bool queued = false;

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel.
    if (socket_ == INVALID_SOCKET)
    {
        err = WSAEBADF;
    }
    else
    {
        // Prepare buffers
        capy::mutable_buffer bufs[write_op::max_buffers];
        op.wsabuf_count =
            static_cast<DWORD>(param.copy_to(bufs, write_op::max_buffers));

        // An empty buffer completes immediately with 0 bytes.
        if (op.wsabuf_count != 0)
        {
            for (DWORD i = 0; i < op.wsabuf_count; ++i)
            {
                op.wsabufs[i].buf = static_cast<char*>(bufs[i].data());
                op.wsabufs[i].len = static_cast<ULONG>(
                    (std::min)(bufs[i].size(), std::size_t(0x7fffffff)));
            }

            queued = queued_after(
                ::WSASend(
                    socket_, op.wsabufs, op.wsabuf_count, &bytes, 0, &op,
                    nullptr),
                err);
        }
    }

    if (!queued)
        return complete_now(op, cont, d, token, ec, bytes_out, err, bytes);

    return pend(op, cont, d, token, ec, bytes_out);
}

inline std::coroutine_handle<>
win_tcp_socket::wait(
    capy::continuation& cont,
    capy::executor_ref d,
    wait_type w,
    std::stop_token token,
    std::error_code* ec)
{
    wt_.object_ref_ = detail::object_ref(this);

    auto& op = wt_;
    op.reset();
    op.cont         = &cont;
    op.ex           = d;
    op.ec_out       = ec;
    op.bytes_out    = nullptr;
    op.empty_buffer = true; // skip EOF translation in invoke_handler
    op.w            = w;
    op.start(token);

    svc_.work_started();

    // Closed-object contract: complete with bad_file_descriptor
    // without touching the kernel or the wait reactor.
    if (socket_ == INVALID_SOCKET)
    {
        svc_.on_completion(&op, WSAEBADF, 0);
        return std::noop_coroutine();
    }

    if (w == wait_type::read)
    {
        // Zero-byte WSARecv: kernel signals completion when data is
        // available without consuming any bytes from the stream. This
        // is the documented Winsock pattern for "is the socket
        // readable" notifications.
        op.wsabuf = WSABUF{0, nullptr};
        op.flags  = 0;

        int result =
            ::WSARecv(socket_, &op.wsabuf, 1, nullptr, &op.flags, &op, nullptr);

        if (result == SOCKET_ERROR)
        {
            DWORD err = ::WSAGetLastError();
            if (err != WSA_IO_PENDING)
            {
                svc_.on_completion(&op, err, 0);
                return std::noop_coroutine();
            }
        }
        else if (skip_)
        {
            // Readable already, and this socket queues no packet for
            // a request that succeeds during the call.
            svc_.on_completion(&op, 0, 0);
            return std::noop_coroutine();
        }

        // Must precede on_pending: once it runs another thread may
        // complete the op and recycle this impl.
        if (op.cancelled.load(std::memory_order_acquire))
            ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), &op);

        svc_.on_pending(&op);

        return std::noop_coroutine();
    }

    // wait_type::write and wait_type::error: route through the
    // auxiliary poll reactor. There is no overlapped primitive for
    // "the send buffer has room" that does not also transfer bytes,
    // and a write wait must report real writability.
    svc_.scheduler().wait_reactor().register_wait(socket_, w, &op);
    return std::noop_coroutine();
}

inline void
win_tcp_socket::cancel() noexcept
{
    if (socket_ != INVALID_SOCKET)
    {
        ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }

    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    wt_.request_cancel();
    // CancelIoEx covers overlapped I/O on the socket but cannot reach
    // a wait op parked in the auxiliary reactor (no overlapped is
    // outstanding). Route through the reactor explicitly.
    svc_.scheduler().cancel_wait(&wt_);
}

inline void
win_tcp_socket::close_socket() noexcept
{
    // Flag every op cancelled before tearing down the handle. closesocket()
    // can complete a pending overlapped op with ERROR_NETNAME_DELETED (not the
    // MSDN-documented ERROR_OPERATION_ABORTED), which iocp_make_err now maps to
    // connection_reset -- so a locally-closed op must be short-circuited to
    // canceled via the flag rather than relying on the raw error, exactly as
    // cancel() does. The store happens-before the completion is decoded.
    conn_.request_cancel();
    rd_.request_cancel();
    wr_.request_cancel();
    // wt_ is parked in the aux reactor (no overlapped outstanding), so it also
    // needs an explicit reactor deregister before the SOCKET handle is closed;
    // otherwise the reactor would keep polling a dangling fd (and on a Winsock
    // SOCKET-id reuse the wrong fd could be polled briefly).
    wt_.request_cancel();
    svc_.scheduler().cancel_wait(&wt_);

    if (socket_ != INVALID_SOCKET)
    {
        // closesocket() aborts pending I/O only when it closes the last
        // handle to the socket; with an inherited or duplicated handle
        // still open, an op left in the kernel would never complete. So
        // cancel explicitly, but only when an op is in the kernel: the
        // call costs every close otherwise. That test reads what the
        // provider wrote in the OVERLAPPED, which only a provider of
        // kernel handles is known to write; behind any other the
        // socket is always cancelled. wt_'s write and error waits
        // live in the wait reactor, released above.
        if (!ifs_ || kernel_pending(conn_) || kernel_pending(rd_) ||
            kernel_pending(wr_) || kernel_pending(wt_))
            ::CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    family_ = AF_UNSPEC;
    skip_   = false;
    ifs_    = false;

    // Clear cached endpoints
    local_endpoint_  = endpoint{};
    remote_endpoint_ = endpoint{};
    local_state_.store(local_state::cached, std::memory_order_release);
}

inline std::error_code
win_tcp_socket::shutdown(tcp_socket::shutdown_type what) noexcept
{
    int how;
    switch (what)
    {
    case tcp_socket::shutdown_receive:
        how = SD_RECEIVE;
        break;
    case tcp_socket::shutdown_send:
        how = SD_SEND;
        break;
    case tcp_socket::shutdown_both:
        how = SD_BOTH;
        break;
    default:
        return make_err(WSAEINVAL);
    }
    if (::shutdown(socket_, how) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline native_handle_type
win_tcp_socket::native_handle() const noexcept
{
    return static_cast<native_handle_type>(socket_);
}

inline native_handle_type
win_tcp_socket::release_socket() noexcept
{
    SOCKET s = socket_;
    if (s != INVALID_SOCKET)
    {
        cancel();
        // Sever the port association so the descriptor can be
        // adopted again; best-effort, the caller keeps a working
        // socket either way.
        dissociate_from_iocp(s);
        socket_          = INVALID_SOCKET;
        family_          = AF_UNSPEC;
        skip_            = false;
        ifs_             = false;
        local_endpoint_  = endpoint{};
        remote_endpoint_ = endpoint{};
        local_state_.store(local_state::cached, std::memory_order_release);
    }
    return static_cast<native_handle_type>(s);
}

inline std::error_code
win_tcp_socket::set_option(
    int level, int optname, void const* data, std::size_t size) noexcept
{
    if (::setsockopt(
            socket_, level, optname, reinterpret_cast<char const*>(data),
            static_cast<int>(size)) != 0)
        return make_err(WSAGetLastError());
    return {};
}

inline std::error_code
win_tcp_socket::get_option(
    int level, int optname, void* data, std::size_t* size) const noexcept
{
    int len = static_cast<int>(*size);
    if (::getsockopt(
            socket_, level, optname, reinterpret_cast<char*>(data), &len) != 0)
        return make_err(WSAGetLastError());
    *size = static_cast<std::size_t>(len);
    return {};
}

inline endpoint
win_tcp_socket::local_endpoint() const noexcept
{
    auto state = local_state_.load(std::memory_order_acquire);
    if (state == local_state::cached)
        return local_endpoint_;

    // A connect left the endpoint for the first read to fetch. One
    // reader claims the cache and fills it; a reader racing it asks the
    // kernel for its own copy rather than read the cache mid-write. A
    // fetch that fails caches nothing, so the next read asks again.
    sockaddr_storage storage{};
    int len = sizeof(storage);
    if (socket_ == INVALID_SOCKET ||
        ::getsockname(socket_, reinterpret_cast<sockaddr*>(&storage), &len) !=
            0)
        return endpoint{};
    endpoint const ep = from_sockaddr(storage);
    if (state == local_state::pending &&
        local_state_.compare_exchange_strong(
            state, local_state::fetching, std::memory_order_acquire,
            std::memory_order_relaxed))
    {
        local_endpoint_ = ep;
        local_state_.store(local_state::cached, std::memory_order_release);
    }
    return ep;
}

inline endpoint
win_tcp_socket::remote_endpoint() const noexcept
{
    return remote_endpoint_;
}

// win_tcp_service

inline win_tcp_service::win_tcp_service(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
    , iocp_(sched_.native_handle())
{
    load_extension_functions();
    ifs_v4_ = provider_is_ifs(AF_INET);
    ifs_v6_ = provider_is_ifs(AF_INET6);
}

inline void
win_tcp_service::shutdown()
{
    // Close all sockets/acceptors to force pending I/O to complete via
    // IOCP. Not deleted here: in-flight ops hold their own object_ref, so
    // each impl stays alive until its cancel completion drains; the
    // pools' own destructors free whatever is still live once this
    // service itself is torn down (after the scheduler, which drains
    // every queued completion first).
    pool_.shutdown([](win_tcp_socket* impl) { impl->close_socket(); });

    acceptor_pool_.shutdown(
        [](win_tcp_acceptor* impl) { impl->close_socket(); });
}

inline io_object::implementation*
win_tcp_service::construct()
{
    return pool_.acquire(*this);
}

inline void
win_tcp_service::destroy(io_object::implementation* p)
{
    if (!p)
        return;
    auto* s = static_cast<win_tcp_socket*>(p);
    s->close_socket();
    release(s);
}

inline void
win_tcp_service::close(io_object::handle& h)
{
    static_cast<win_tcp_socket*>(h.get())->close_socket();
}

inline std::error_code
win_tcp_service::open_socket(
    win_tcp_socket& impl, int family, int type, int protocol)
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
    impl.skip_   = enable_skip_on_success(sock, family);
    impl.ifs_    = family == AF_INET6 ? ifs_v6_ : ifs_v4_;
    return {};
}

inline bool
win_tcp_service::enable_skip_on_success(SOCKET s, int family) const noexcept
{
    if (!iocp_skip_on_success_enabled() ||
        !(family == AF_INET6 ? ifs_v6_ : ifs_v4_))
        return false;
    // FILE_SKIP_SET_EVENT_ON_HANDLE is left alone: it is as permanent
    // as this mode, and a socket released from here may go to a
    // caller who waits on the handle for its overlapped results.
    return ::SetFileCompletionNotificationModes(
               reinterpret_cast<HANDLE>(s),
               FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) != 0;
}

inline bool
win_tcp_service::provider_is_ifs(int family) noexcept
{
    SOCKET sock = ::WSASocketW(
        family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET)
        return false;
    WSAPROTOCOL_INFOW info{};
    int len       = sizeof(info);
    bool const ok = ::getsockopt(
                        sock, SOL_SOCKET, SO_PROTOCOL_INFOW,
                        reinterpret_cast<char*>(&info), &len) == 0 &&
        (info.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;
    ::closesocket(sock);
    return ok;
}

inline std::error_code
win_tcp_service::assign_socket(win_tcp_socket& impl, native_handle_type fd)
{
    SOCKET sock = static_cast<SOCKET>(fd);
    if (sock == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);

    // SO_PROTOCOL_INFOW works on an unbound socket, unlike getsockname
    // (WSAEINVAL until bind/connect names it) -- an adopted socket may
    // have reached connected state without an explicit bind.
    WSAPROTOCOL_INFOW proto_info{};
    int proto_len = sizeof(proto_info);
    if (::getsockopt(
            sock, SOL_SOCKET, SO_PROTOCOL_INFOW,
            reinterpret_cast<char*>(&proto_info), &proto_len) != 0)
        return make_err(::WSAGetLastError());
    if (proto_info.iAddressFamily != AF_INET &&
        proto_info.iAddressFamily != AF_INET6)
        return make_err(WSAEAFNOSUPPORT);
    if (proto_info.iSocketType != SOCK_STREAM)
        return make_err(WSAEPROTOTYPE);

    // A socket already in skip-on-success mode is taken as it is: the
    // mode cannot be cleared, and it is the one a socket released
    // from this library carries. It only delivers completions when
    // its provider hands out real kernel handles; behind any other
    // provider the socket would hang, so that one is refused. The
    // mode is never set on a socket adopted without it, which stays
    // the caller's to hand elsewhere.
    bool const skip = has_skip_on_success(reinterpret_cast<HANDLE>(sock));
    if (skip && (proto_info.dwServiceFlags1 & XP1_IFS_HANDLES) == 0)
        return std::make_error_code(std::errc::operation_not_supported);

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);
    if (result == nullptr)
        return make_err(::GetLastError());

    impl.socket_ = sock;
    impl.family_ = proto_info.iAddressFamily;
    impl.skip_   = skip;
    impl.ifs_    = (proto_info.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;

    endpoint local_ep, remote_ep;
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        local_ep = detail::from_sockaddr(local_storage);
    sockaddr_storage remote_storage{};
    int remote_len = sizeof(remote_storage);
    if (::getpeername(
            sock, reinterpret_cast<sockaddr*>(&remote_storage), &remote_len) ==
        0)
        remote_ep = detail::from_sockaddr(remote_storage);
    impl.set_endpoints(local_ep, remote_ep);

    return {};
}

inline std::error_code
win_tcp_service::bind_socket(win_tcp_socket& impl, endpoint ep)
{
    SOCKET sock = impl.socket_;

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
        impl.local_endpoint_ = detail::from_sockaddr(local_storage);
    impl.local_state_.store(
        win_tcp_socket::local_state::cached, std::memory_order_release);

    return {};
}

inline void*
win_tcp_service::native_handle() const noexcept
{
    return iocp_;
}

inline void
win_tcp_service::post(overlapped_op* op)
{
    sched_.post(op);
}

inline void
win_tcp_service::on_pending(overlapped_op* op) noexcept
{
    sched_.on_pending(op);
}

inline void
win_tcp_service::on_completion(
    overlapped_op* op, DWORD error, DWORD bytes) noexcept
{
    sched_.on_completion(op, error, bytes);
}

inline void
win_tcp_service::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_tcp_service::work_finished() noexcept
{
    sched_.work_finished();
}

inline void
win_tcp_service::load_extension_functions()
{
    SOCKET sock = ::WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return;

    DWORD bytes = 0;

    GUID connect_ex_guid = WSAID_CONNECTEX;
    ::WSAIoctl(
        sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_ex_guid,
        sizeof(connect_ex_guid), &connect_ex_, sizeof(connect_ex_), &bytes,
        nullptr, nullptr);

    GUID accept_ex_guid = WSAID_ACCEPTEX;
    ::WSAIoctl(
        sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_ex_guid,
        sizeof(accept_ex_guid), &accept_ex_, sizeof(accept_ex_), &bytes,
        nullptr, nullptr);

    GUID sockaddrs_guid = WSAID_GETACCEPTEXSOCKADDRS;
    ::WSAIoctl(
        sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &sockaddrs_guid,
        sizeof(sockaddrs_guid), &get_accept_ex_sockaddrs_,
        sizeof(get_accept_ex_sockaddrs_), &bytes, nullptr, nullptr);

    ::closesocket(sock);
}

inline win_tcp_acceptor*
win_tcp_service::acquire_acceptor_impl()
{
    return acceptor_pool_.acquire(*this);
}

inline std::error_code
win_tcp_service::open_acceptor_socket(
    win_tcp_acceptor& impl, int family, int type, int protocol)
{
    impl.close_socket();

    SOCKET sock =
        ::WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);

    if (sock == INVALID_SOCKET)
        return make_err(::WSAGetLastError());

    if (family == AF_INET6)
    {
        DWORD val = 0; // dual-stack default
        ::setsockopt(
            sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&val),
            sizeof(val));
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
    impl.ifs_    = family == AF_INET6 ? ifs_v6_ : ifs_v4_;
    return {};
}

inline std::error_code
win_tcp_service::assign_acceptor_socket(
    win_tcp_acceptor& impl, native_handle_type fd)
{
    SOCKET sock = static_cast<SOCKET>(fd);
    if (sock == INVALID_SOCKET)
        return make_err(WSAENOTSOCK);

    // SO_PROTOCOL_INFOW works on an unbound socket, unlike getsockname
    // (WSAEINVAL until bind names it).
    WSAPROTOCOL_INFOW proto_info{};
    int proto_len = sizeof(proto_info);
    if (::getsockopt(
            sock, SOL_SOCKET, SO_PROTOCOL_INFOW,
            reinterpret_cast<char*>(&proto_info), &proto_len) != 0)
        return make_err(::WSAGetLastError());
    if (proto_info.iAddressFamily != AF_INET &&
        proto_info.iAddressFamily != AF_INET6)
        return make_err(WSAEAFNOSUPPORT);
    if (proto_info.iSocketType != SOCK_STREAM)
        return make_err(WSAEPROTOTYPE);

    if (has_skip_on_success(reinterpret_cast<HANDLE>(sock)))
        return std::make_error_code(std::errc::operation_not_supported);

    HANDLE result = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(sock), static_cast<HANDLE>(iocp_), key_io, 0);
    if (result == nullptr)
        return make_err(::GetLastError());

    impl.socket_ = sock;
    impl.family_ = proto_info.iAddressFamily;
    impl.ifs_    = (proto_info.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;

    // AcceptEx sizes its address buffers from this cache, so an
    // unseeded endpoint breaks accepts on an adopted v6 listener.
    sockaddr_storage local_storage{};
    int local_len = sizeof(local_storage);
    if (::getsockname(
            sock, reinterpret_cast<sockaddr*>(&local_storage), &local_len) == 0)
        impl.set_local_endpoint(detail::from_sockaddr(local_storage));

    return {};
}

inline std::error_code
win_tcp_service::bind_acceptor(win_tcp_acceptor& impl, endpoint ep)
{
    SOCKET sock = impl.socket_;

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
        impl.set_local_endpoint(detail::from_sockaddr(local_storage));

    return {};
}

inline std::error_code
win_tcp_service::listen_acceptor(win_tcp_acceptor& impl, int backlog)
{
    SOCKET sock = impl.socket_;

    if (::listen(sock, backlog) == SOCKET_ERROR)
        return make_err(::WSAGetLastError());

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TCP_SERVICE_HPP
