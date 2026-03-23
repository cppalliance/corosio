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
    from reactor_basic_socket. Adds the TCP-specific remote
    endpoint, shutdown, and I/O dispatch (connect, read, write).

    @tparam Derived   The concrete socket type (CRTP).
    @tparam Service   The backend's socket service type.
    @tparam ConnOp    The backend's connect op type.
    @tparam ReadOp    The backend's read op type.
    @tparam WriteOp   The backend's write op type.
    @tparam DescState The backend's descriptor_state type.
*/
template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
class reactor_stream_socket
    : public reactor_basic_socket<
          Derived,
          tcp_socket::implementation,
          Service,
          DescState>
{
    using base_type = reactor_basic_socket<
        Derived,
        tcp_socket::implementation,
        Service,
        DescState>;
    friend base_type;
    friend Derived;

    explicit reactor_stream_socket(Service& svc) noexcept : base_type(svc) {}

protected:
    endpoint remote_endpoint_;

public:
    /// Pending connect operation slot.
    ConnOp conn_;

    /// Pending read operation slot.
    ReadOp rd_;

    /// Pending write operation slot.
    WriteOp wr_;

    ~reactor_stream_socket() override = default;

    /// Return the cached remote endpoint.
    endpoint remote_endpoint() const noexcept override
    {
        return remote_endpoint_;
    }

    /// Shut down part or all of the full-duplex connection.
    std::error_code shutdown(tcp_socket::shutdown_type what) noexcept override
    {
        int how;
        switch (what)
        {
        case tcp_socket::shutdown_receive:
            how = SHUT_RD;
            break;
        case tcp_socket::shutdown_send:
            how = SHUT_WR;
            break;
        case tcp_socket::shutdown_both:
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
    void set_endpoints(endpoint local, endpoint remote) noexcept
    {
        this->local_endpoint_ = local;
        remote_endpoint_      = remote;
    }

    /** Shared connect dispatch.

        Tries the connect syscall speculatively. On synchronous
        completion, returns via inline budget or posts through queue.
        On EINPROGRESS, registers with the reactor.
    */
    std::coroutine_handle<> do_connect(
        std::coroutine_handle<>,
        capy::executor_ref,
        endpoint,
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
        remote_endpoint_ = endpoint{};
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
    class DescState>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState>::
    do_connect(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        endpoint ep,
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
            this->local_endpoint_ = from_sockaddr(local_storage);
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

template<
    class Derived,
    class Service,
    class ConnOp,
    class ReadOp,
    class WriteOp,
    class DescState>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState>::
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

    // Speculative read
    ssize_t n;
    do
    {
        n = ::readv(this->fd_, op.iovecs, op.iovec_count);
    }
    while (n < 0 && errno == EINTR);

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
    class DescState>
std::coroutine_handle<>
reactor_stream_socket<Derived, Service, ConnOp, ReadOp, WriteOp, DescState>::
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

    // Speculative write via backend-specific write policy
    ssize_t n =
        WriteOp::write_policy::write(this->fd_, op.iovecs, op.iovec_count);

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
        this->desc_state_.write_cancel_pending);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_SOCKET_HPP
