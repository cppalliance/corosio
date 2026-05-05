//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_SOCKET_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_SOCKET_HPP

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/native/detail/reactor/reactor_basic_socket.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>

#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed stream socket implementations.

    Inherits shared data members and cancel/close/register logic
    from reactor_basic_socket. Adds the stream-specific remote
    endpoint, shutdown, and I/O dispatch (connect, read, write).

    @tparam Derived   The concrete socket type (CRTP).
    @tparam Service   The backend's socket service type.
    @tparam ConnOp    The backend's connect op type.
    @tparam ReadOp    The backend's read op type.
    @tparam WriteOp   The backend's write op type.
    @tparam DescState The backend's descriptor_state type.
    @tparam ImplBase  The public vtable base
                      (tcp_socket::implementation or
                       local_stream_socket::implementation).
    @tparam Endpoint  The endpoint type (endpoint or local_endpoint).
*/
template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState,
    class ImplBase = tcp_socket::implementation,
    class Endpoint = endpoint>
class reactor_stream_socket
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
    friend base_type;
    friend Derived;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_stream_socket(Service& svc) noexcept : base_type(svc) {}

protected:
    Endpoint remote_endpoint_;

public:
    /// Pending connect operation slot.
    ConnOp conn_;

    /// Pending read operation slot.
    ReadOp rd_;

    /// Pending write operation slot.
    WriteOp wr_;

    ~reactor_stream_socket() override = default;

    /// Return the cached remote endpoint.
    Endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

    // --- Virtual method overrides (satisfy ImplBase pure virtuals) ---

    std::coroutine_handle<> connect(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        Endpoint ep,
        std::stop_token token,
        std::error_code* ec) override
    {
        return do_connect(h, ex, ep, token, ec);
    }

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_read_some(h, ex, param, token, ec, bytes_out);
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes_out) override
    {
        return do_write_some(h, ex, param, token, ec, bytes_out);
    }

    std::error_code
    shutdown(corosio::shutdown_type what) noexcept override
    {
        return do_shutdown(static_cast<int>(what));
    }

    void cancel() noexcept override
    {
        this->do_cancel();
    }

    // --- End virtual overrides ---

    /// Close the socket (non-virtual, called by the service).
    void close_socket() noexcept
    {
        this->do_close_socket();
    }

    /** Shut down part or all of the full-duplex connection.

        @param what 0 = receive, 1 = send, 2 = both.
    */
    std::error_code do_shutdown(int what) noexcept
    {
        int how;
        switch (what)
        {
        case 0: // shutdown_receive
            how = SHUT_RD;
            break;
        case 1: // shutdown_send
            how = SHUT_WR;
            break;
        case 2: // shutdown_both
            how = SHUT_RDWR;
            break;
        default:
            return make_err(EINVAL);
        }
        if (::shutdown(this->fd_, how) != 0)
            return make_err(errno);
        return {};
    }

    /// Cache local and remote endpoints.
    void set_endpoints(Endpoint local, Endpoint remote) noexcept
    {
        this->local_endpoint_ = std::move(local);
        remote_endpoint_      = std::move(remote);
    }

    /** Shared connect dispatch.

        Tries the connect syscall speculatively. On synchronous
        completion, returns via inline budget or posts through queue.
        On EINPROGRESS, registers with the reactor.
    */
    std::coroutine_handle<> do_connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        Endpoint const&,
        std::stop_token const&,
        std::error_code*);

    /** Shared scatter-read dispatch.

        Tries readv() speculatively. On success or hard error,
        returns via inline budget or posts through queue.
        On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Shared gather-write dispatch.

        Tries the write via WriteOp::write_policy speculatively.
        On success or hard error, returns via inline budget or
        posts through queue. On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

    /** Close the socket and cancel pending operations.

        Extends the base do_close_socket() to also reset
        the remote endpoint.
    */
    void do_close_socket() noexcept
    {
        base_type::do_close_socket();
        remote_endpoint_ = Endpoint{};
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
        return nullptr;
    }

    template<class Fn>
    void for_each_op(Fn fn) noexcept
    {
        fn(conn_);
        fn(rd_);
        fn(wr_);
    }

    template<class Fn>
    void for_each_desc_entry(Fn fn) noexcept
    {
        fn(conn_, this->desc_state_.connect_op);
        fn(rd_, this->desc_state_.read_op);
        fn(wr_, this->desc_state_.write_op);
    }
};

template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState, ImplBase, Endpoint>::
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
        this->desc_state_.connect_cancel_pending, true);
    return std::noop_coroutine();
}

template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState, ImplBase, Endpoint>::
    do_read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = rd_;
    op.reset();

    capy::mutable_buffer bufs[ReadOp::max_buffers];
    op.iovec_count = static_cast<int>(param.copy_to(bufs, ReadOp::max_buffers));

    if (op.iovec_count == 0 || (op.iovec_count == 1 && bufs[0].size() == 0))
    {
        op.empty_buffer_read = true;
        op.h                 = h;
        op.ex                = ex;
        op.ec_out            = ec;
        op.bytes_out         = bytes_out;
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

    // Speculative read; for the single-buffer case use recv() so the
    // kernel skips the readv iov_iter setup.
    ssize_t n;
    if (op.iovec_count == 1)
    {
        do
        {
            n = ::recv(this->fd_, bufs[0].data(), bufs[0].size(), 0);
        }
        while (n < 0 && errno == EINTR);
    }
    else
    {
        do
        {
            n = ::readv(this->fd_, op.iovecs, op.iovec_count);
        }
        while (n < 0 && errno == EINTR);
    }

    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
    {
        int err    = (n < 0) ? errno : 0;
        auto bytes = (n > 0) ? static_cast<std::size_t>(n) : std::size_t(0);

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            if (err)
                *ec = make_err(err);
            else if (n == 0)
                *ec = capy::error::eof;
            else
                *ec = {};
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
    op.fd        = this->fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.read_op, this->desc_state_.read_ready,
        this->desc_state_.read_cancel_pending);
    return std::noop_coroutine();
}

template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState,
    class ImplBase,
    class Endpoint>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState, ImplBase, Endpoint>::
    do_write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[WriteOp::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, WriteOp::max_buffers));

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

    // Speculative write; the single-buffer case dispatches to a
    // backend-specific fast path so the kernel skips msghdr/iov_iter
    // setup (and so each backend can pick the right SIGPIPE strategy).
    ssize_t n;
    if (op.iovec_count == 1)
    {
        n = WriteOp::write_policy::write_one(
            this->fd_, bufs[0].data(), bufs[0].size());
    }
    else
    {
        n = WriteOp::write_policy::write(
            this->fd_, op.iovecs, op.iovec_count);
    }

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
    op.fd        = this->fd_;
    op.start(token, static_cast<Derived*>(this));
    op.impl_ptr = this->shared_from_this();

    this->register_op(
        op, this->desc_state_.write_op, this->desc_state_.write_ready,
        this->desc_state_.write_cancel_pending, true);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_SOCKET_HPP
