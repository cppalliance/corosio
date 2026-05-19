//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_SOCKET_HPP

#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/native/detail/reactor/reactor_basic_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>

#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/* Map portable message_flags values to native MSG_* constants. */
inline int
to_native_msg_flags(int flags) noexcept
{
    int native = 0;
    if (flags & 1) native |= MSG_PEEK;
    if (flags & 2) native |= MSG_OOB;
    if (flags & 4) native |= MSG_DONTROUTE;
    return native;
}

/** CRTP base for reactor-backed datagram socket implementations.

    Inherits shared data members and cancel/close/register logic
    from reactor_basic_socket. Adds datagram-specific I/O dispatch
    for both connectionless (send_to, recv_from) and connected
    (connect, send, recv) modes.

    @tparam Derived    The concrete socket type (CRTP).
    @tparam Service    The backend's datagram service type.
    @tparam ConnOp     The backend's connect op type.
    @tparam SendToOp   The backend's send_to op type.
    @tparam RecvFromOp The backend's recv_from op type.
    @tparam SendOp     The backend's connected send op type.
    @tparam RecvOp     The backend's connected recv op type.
    @tparam WaitOp     The backend's wait op type.
    @tparam DescState  The backend's descriptor_state type.
    @tparam ImplBase   The public vtable base
                       (udp_socket::implementation or
                        local_datagram_socket::implementation).
    @tparam Endpoint   The endpoint type (endpoint or local_endpoint).
*/
template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase = udp_socket::implementation,
    class Endpoint = endpoint>
class reactor_datagram_socket
    : public reactor_basic_socket<
          Derived,
          ImplBase,
          Service,
          DescState,
          Endpoint>
{
    using base_type = reactor_basic_socket<
        Derived,
        ImplBase,
        Service,
        DescState,
        Endpoint>;
    using self_type = reactor_datagram_socket<
        Derived, Service, ConnOp, SendToOp, RecvFromOp, SendOp, RecvOp, WaitOp,
        DescState, ImplBase, Endpoint>;
    friend base_type;
    friend Derived;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_datagram_socket(Service& svc) noexcept : base_type(svc) {}

protected:
    Endpoint remote_endpoint_;

public:
    /// Pending connect operation slot.
    ConnOp conn_;

    /// Pending send_to operation slot.
    SendToOp wr_;

    /// Pending recv_from operation slot.
    RecvFromOp rd_;

    /// Pending connected send operation slot.
    SendOp send_wr_;

    /// Pending connected recv operation slot.
    RecvOp recv_rd_;

    /// Pending wait-for-read operation slot.
    WaitOp wait_rd_;

    /// Pending wait-for-write operation slot.
    WaitOp wait_wr_;

    /// Pending wait-for-error operation slot.
    WaitOp wait_er_;

    ~reactor_datagram_socket() override = default;

    /// Return the cached remote endpoint.
    Endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

    // --- Virtual method overrides (satisfy ImplBase pure virtuals) ---

    std::coroutine_handle<> send_to(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        Endpoint dest,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_send_to(h, ex, buf, dest, flags, token, ec, bytes_out);
    }

    std::coroutine_handle<> recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        Endpoint* source,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_recv_from(h, ex, buf, source, flags, token, ec, bytes_out);
    }

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        Endpoint ep,
        std::stop_token token,
        std::error_code* ec) override
    {
        return do_connect(h, ex, ep, token, ec);
    }

    std::coroutine_handle<> send(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_send(h, ex, buf, flags, token, ec, bytes_out);
    }

    std::coroutine_handle<> recv(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        int flags,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_recv(h, ex, buf, flags, token, ec, bytes_out);
    }

    void cancel() noexcept override
    {
        this->do_cancel();
    }

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override
    {
        return do_wait(h, ex, w, token, ec);
    }

    // --- End virtual overrides ---

    /// Close the socket (non-virtual, called by the service).
    void close_socket() noexcept
    {
        do_close_socket();
    }

    /// Cache local and remote endpoints.
    void set_endpoints(Endpoint local, Endpoint remote) noexcept
    {
        this->local_endpoint_ = std::move(local);
        remote_endpoint_      = std::move(remote);
    }

    /** Shared send_to dispatch.

        Tries sendmsg() speculatively. On success or hard error,
        returns via inline budget or posts through queue.
        On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        Endpoint const&,
        int flags,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared recv_from dispatch.

        Tries recvmsg() speculatively. On success or hard error,
        returns via inline budget or posts through queue.
        On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_recv_from(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        Endpoint*,
        int flags,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared connect dispatch.

        Tries connect() speculatively. On synchronous completion,
        returns via inline budget or posts through queue.
        On EINPROGRESS, registers with the reactor.
    */
    std::coroutine_handle<> do_connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        Endpoint const&,
        std::stop_token const&,
        std::error_code*);

    /** Shared connected send dispatch.

        Like do_send_to but uses send_wr_ slot and sendmsg()
        with msg_name=nullptr.
    */
    std::coroutine_handle<> do_send(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared connected recv dispatch.

        Like do_recv_from but uses recv_rd_ slot and recvmsg()
        with msg_name=nullptr.
    */
    std::coroutine_handle<> do_recv(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        int flags,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared readiness-wait dispatch.

        Registers a wait op for the requested direction. Does not
        perform any I/O syscall — completion is signalled when the
        reactor delivers the matching edge event.
    */
    std::coroutine_handle<> do_wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        wait_type,
        std::stop_token const&,
        std::error_code*);

    /** Close the socket and cancel pending operations.

        Extends the base do_close_socket() to also reset
        the remote endpoint.
    */
    void do_close_socket() noexcept
    {
        base_type::do_close_socket();
        remote_endpoint_ = Endpoint{};
    }

    native_handle_type do_release_socket() noexcept
    {
        auto fd = base_type::do_release_socket();
        remote_endpoint_ = Endpoint{};
        return fd;
    }

    /** Shut down part or all of the full-duplex connection.

        Not an override — concrete backends forward here.

        @param what 0 = receive, 1 = send, 2 = both.
    */
    std::error_code do_shutdown(int what) noexcept
    {
        int how;
        switch (what)
        {
        case 0:
            how = SHUT_RD;
            break;
        case 1:
            how = SHUT_WR;
            break;
        case 2:
            how = SHUT_RDWR;
            break;
        default:
            return make_err(EINVAL);
        }
        if (::shutdown(this->fd_, how) != 0)
            return make_err(errno);
        return {};
    }

private:
    // CRTP callbacks for reactor_basic_socket cancel/close

    template<class Op>
    reactor_op_base** op_to_desc_slot(Op& op) noexcept
    {
        if (&op == static_cast<void*>(&conn_))
            return &this->desc_state_.connect_op;
        if (&op == static_cast<void*>(&rd_))
            return &this->desc_state_.read_op;
        if (&op == static_cast<void*>(&wr_))
            return &this->desc_state_.write_op;
        if (&op == static_cast<void*>(&recv_rd_))
            return &this->desc_state_.read_op;
        if (&op == static_cast<void*>(&send_wr_))
            return &this->desc_state_.write_op;
        if (&op == static_cast<void*>(&wait_rd_))
            return &this->desc_state_.wait_read_op;
        if (&op == static_cast<void*>(&wait_wr_))
            return &this->desc_state_.wait_write_op;
        if (&op == static_cast<void*>(&wait_er_))
            return &this->desc_state_.wait_error_op;
        return nullptr;
    }

    template<class Op>
    bool* op_to_cancel_flag(Op& op) noexcept
    {
        if (&op == static_cast<void*>(&conn_))
            return &this->desc_state_.connect_cancel_pending;
        if (&op == static_cast<void*>(&rd_))
            return &this->desc_state_.read_cancel_pending;
        if (&op == static_cast<void*>(&wr_))
            return &this->desc_state_.write_cancel_pending;
        if (&op == static_cast<void*>(&recv_rd_))
            return &this->desc_state_.read_cancel_pending;
        if (&op == static_cast<void*>(&send_wr_))
            return &this->desc_state_.write_cancel_pending;
        if (&op == static_cast<void*>(&wait_rd_))
            return &this->desc_state_.wait_read_cancel_pending;
        if (&op == static_cast<void*>(&wait_wr_))
            return &this->desc_state_.wait_write_cancel_pending;
        if (&op == static_cast<void*>(&wait_er_))
            return &this->desc_state_.wait_error_cancel_pending;
        return nullptr;
    }

    template<class Fn>
    void for_each_op(Fn fn) noexcept
    {
        fn(conn_);
        fn(rd_);
        fn(wr_);
        fn(recv_rd_);
        fn(send_wr_);
        fn(wait_rd_);
        fn(wait_wr_);
        fn(wait_er_);
    }

    template<class Fn>
    void for_each_desc_entry(Fn fn) noexcept
    {
        fn(conn_, this->desc_state_.connect_op);
        fn(rd_, this->desc_state_.read_op);
        fn(wr_, this->desc_state_.write_op);
        fn(recv_rd_, this->desc_state_.read_op);
        fn(send_wr_, this->desc_state_.write_op);
        fn(wait_rd_, this->desc_state_.wait_read_op);
        fn(wait_wr_, this->desc_state_.wait_write_op);
        fn(wait_er_, this->desc_state_.wait_error_op);
    }
};

// do_send_to

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_send_to(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        Endpoint const& dest,
        int flags,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[SendToOp::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, SendToOp::max_buffers));

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    // Set up destination address
    op.dest_len  = to_sockaddr(dest, socket_family(this->fd_), op.dest_storage);
    op.fd        = this->fd_;
    op.msg_flags = to_native_msg_flags(flags);

    // Speculative sendmsg
    msghdr msg{};
    msg.msg_name    = &op.dest_storage;
    msg.msg_namelen = op.dest_len;
    msg.msg_iov     = op.iovecs;
    msg.msg_iovlen  = static_cast<std::size_t>(op.iovec_count);

#ifdef MSG_NOSIGNAL
    int send_flags = op.msg_flags | MSG_NOSIGNAL;
#else
    int send_flags = op.msg_flags;
#endif

    ssize_t n;
    do
    {
        n = ::sendmsg(this->fd_, &msg, send_flags);
    }
    while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.write_op, this->desc_state_.write_ready,
        this->desc_state_.write_cancel_pending, true);
    return std::noop_coroutine();
}

// do_recv_from

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        Endpoint* source,
        int flags,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[RecvFromOp::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, RecvFromOp::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(0, 0);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    op.fd         = this->fd_;
    op.source_out = source;
    op.msg_flags  = to_native_msg_flags(flags);

    // Speculative recvmsg
    msghdr msg{};
    msg.msg_name    = &op.source_storage;
    msg.msg_namelen = sizeof(op.source_storage);
    msg.msg_iov     = op.iovecs;
    msg.msg_iovlen  = static_cast<std::size_t>(op.iovec_count);

    ssize_t n;
    do
    {
        n = ::recvmsg(this->fd_, &msg, op.msg_flags);
    }
    while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);
        if (n >= 0)
            op.source_addrlen = msg.msg_namelen;

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            if (source && !err && n >= 0)
                *source = from_sockaddr_as(
                    op.source_storage,
                    op.source_addrlen,
                    Endpoint{});
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.read_op, this->desc_state_.read_ready,
        this->desc_state_.read_cancel_pending);
    return std::noop_coroutine();
}

// do_connect

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_connect(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        Endpoint const& ep,
        std::stop_token const& token,
        std::error_code* ec)
{
    auto& op = conn_;

    sockaddr_storage storage{};
    socklen_t addrlen = to_sockaddr(ep, socket_family(this->fd_), storage);
    int result =
        ::connect(this->fd_, reinterpret_cast<sockaddr*>(&storage), addrlen);

    if (result == 0)
    {
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                this->fd_, reinterpret_cast<sockaddr*>(&local_storage),
                &local_len) == 0)
            this->local_endpoint_ =
                from_sockaddr_as(local_storage, local_len, Endpoint{});
        remote_endpoint_ = ep;
    }

    if (result == 0 || errno != EINPROGRESS)
    {
        int err = (result < 0) ? errno : 0;
        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec = err ? make_err(err) : std::error_code{};
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.reset();
        op.h               = h;
        op.ex              = ex;
        op.ec_out          = ec;
        op.fd              = this->fd_;
        op.target_endpoint = ep;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, 0);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    // EINPROGRESS — register with reactor
    op.reset();
    op.h               = h;
    op.ex              = ex;
    op.ec_out          = ec;
    op.fd              = this->fd_;
    op.target_endpoint = ep;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.connect_op, this->desc_state_.write_ready,
        this->desc_state_.connect_cancel_pending);
    return std::noop_coroutine();
}

// do_send (connected mode)

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_send(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        int flags,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = send_wr_;
    op.reset();

    capy::mutable_buffer bufs[SendOp::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, SendOp::max_buffers));

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    op.fd        = this->fd_;
    op.msg_flags = to_native_msg_flags(flags);

    // Speculative sendmsg with no destination (connected mode)
    msghdr msg{};
    msg.msg_iov    = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

#ifdef MSG_NOSIGNAL
    int send_flags = op.msg_flags | MSG_NOSIGNAL;
#else
    int send_flags = op.msg_flags;
#endif

    ssize_t n;
    do
    {
        n = ::sendmsg(this->fd_, &msg, send_flags);
    }
    while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.write_op, this->desc_state_.write_ready,
        this->desc_state_.write_cancel_pending, true);
    return std::noop_coroutine();
}

// do_recv (connected mode)

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_recv(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        int flags,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = recv_rd_;
    op.reset();

    capy::mutable_buffer bufs[RecvOp::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, RecvOp::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(0, 0);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    for (int i = 0; i < op.iovec_count; ++i)
    {
        op.iovecs[i].iov_base = bufs[i].data();
        op.iovecs[i].iov_len  = bufs[i].size();
    }

    op.fd        = this->fd_;
    op.msg_flags = to_native_msg_flags(flags);

    // Speculative recvmsg with no source (connected mode)
    msghdr msg{};
    msg.msg_iov    = op.iovecs;
    msg.msg_iovlen = static_cast<std::size_t>(op.iovec_count);

    ssize_t n;
    do
    {
        n = ::recvmsg(this->fd_, &msg, op.msg_flags);
    }
    while (n < 0 && errno == EINTR);

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec        = err ? make_err(err) : std::error_code{};
            *bytes_out = bytes;
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.h         = h;
        op.ex        = ex;
        op.ec_out    = ec;
        op.bytes_out = bytes_out;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr = this->shared_from_this();
        op.complete(err, bytes);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    // EAGAIN — register with reactor
    op.h         = h;
    op.ex        = ex;
    op.ec_out    = ec;
    op.bytes_out = bytes_out;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.read_op, this->desc_state_.read_ready,
        this->desc_state_.read_cancel_pending);
    return std::noop_coroutine();
}

// do_wait

template<
    class Derived,
    class Service,
    class ConnOp,
    class SendToOp,
    class RecvFromOp,
    class SendOp,
    class RecvOp,
    class WaitOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_datagram_socket<
    Derived,
    Service,
    ConnOp,
    SendToOp,
    RecvFromOp,
    SendOp,
    RecvOp,
    WaitOp,
    DescState,
    ImplBase,
    Endpoint>::
    do_wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token const& token,
        std::error_code* ec)
{
    // wait_type::write completes immediately (see reactor_stream_socket::do_wait).
    if (w == wait_type::write)
    {
        auto& op = wait_wr_;
        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            *ec               = std::error_code{};
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }
        op.reset();
        op.wait_event = reactor_event_write;
        op.h          = h;
        op.ex         = ex;
        op.ec_out     = ec;
        op.fd         = this->fd_;
        op.start(token, static_cast<Derived*>(this));
        op.impl_ptr   = this->shared_from_this();
        op.complete(0, 0);
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    WaitOp* op_ptr;
    reactor_op_base** desc_slot_ptr;
    bool* ready_flag_ptr;
    bool* cancel_flag_ptr;
    std::uint32_t event;

    bool dummy_ready = false; // no cached edge for error waits

    if (w == wait_type::read)
    {
        op_ptr          = &wait_rd_;
        desc_slot_ptr   = &this->desc_state_.wait_read_op;
        ready_flag_ptr  = &this->desc_state_.read_ready;
        cancel_flag_ptr = &this->desc_state_.wait_read_cancel_pending;
        event           = reactor_event_read;
    }
    else // wait_type::error
    {
        op_ptr          = &wait_er_;
        desc_slot_ptr   = &this->desc_state_.wait_error_op;
        ready_flag_ptr  = &dummy_ready;
        cancel_flag_ptr = &this->desc_state_.wait_error_cancel_pending;
        event           = reactor_event_error;
    }

    auto& op      = *op_ptr;
    op.reset();
    op.wait_event = event;
    op.h          = h;
    op.ex         = ex;
    op.ec_out     = ec;
    op.fd         = this->fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr   = this->shared_from_this();

    this->register_op(
        op, *desc_slot_ptr, *ready_flag_ptr, *cancel_flag_ptr,
        false);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_SOCKET_HPP
