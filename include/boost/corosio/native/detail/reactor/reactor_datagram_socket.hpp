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
#include <boost/corosio/native/detail/reactor/reactor_basic_socket.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/capy/buffers.hpp>

#include <coroutine>

#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed datagram socket implementations.

    Inherits shared data members and cancel/close/register logic
    from reactor_basic_socket. Adds datagram-specific I/O dispatch
    (send_to, recv_from).

    @tparam Derived    The concrete socket type (CRTP).
    @tparam Service    The backend's datagram service type.
    @tparam SendToOp   The backend's send_to op type.
    @tparam RecvFromOp The backend's recv_from op type.
    @tparam DescState  The backend's descriptor_state type.
*/
template<
    class Derived,
    class Service,
    class SendToOp,
    class RecvFromOp,
    class DescState>
class reactor_datagram_socket
    : public reactor_basic_socket<
          Derived,
          udp_socket::implementation,
          Service,
          DescState>
{
    using base_type = reactor_basic_socket<
        Derived,
        udp_socket::implementation,
        Service,
        DescState>;
    friend base_type;
    friend Derived;

    explicit reactor_datagram_socket(Service& svc) noexcept : base_type(svc) {}

public:
    /// Pending send_to operation slot.
    SendToOp wr_;

    /// Pending recv_from operation slot.
    RecvFromOp rd_;

    ~reactor_datagram_socket() override = default;

    /** Shared send_to dispatch.

        Tries sendmsg() speculatively. On success or hard error,
        returns via inline budget or posts through queue.
        On EAGAIN, registers with the reactor.
    */
    std::coroutine_handle<> do_send_to(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        endpoint,
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
        endpoint*,
        std::stop_token const&,
        std::error_code*,
        std::size_t*);

private:
    // CRTP callbacks for reactor_basic_socket cancel/close

    template<class Op>
    reactor_op_base** op_to_desc_slot(Op& op) noexcept
    {
        if (&op == static_cast<void*>(&rd_))
            return &this->desc_state_.read_op;
        if (&op == static_cast<void*>(&wr_))
            return &this->desc_state_.write_op;
        return nullptr;
    }

    template<class Op>
    bool* op_to_cancel_flag(Op& op) noexcept
    {
        if (&op == static_cast<void*>(&rd_))
            return &this->desc_state_.read_cancel_pending;
        if (&op == static_cast<void*>(&wr_))
            return &this->desc_state_.write_cancel_pending;
        return nullptr;
    }

    template<class Fn>
    void for_each_op(Fn fn) noexcept
    {
        fn(rd_);
        fn(wr_);
    }

    template<class Fn>
    void for_each_desc_entry(Fn fn) noexcept
    {
        fn(rd_, this->desc_state_.read_op);
        fn(wr_, this->desc_state_.write_op);
    }
};

template<
    class Derived,
    class Service,
    class SendToOp,
    class RecvFromOp,
    class DescState>
std::coroutine_handle<>
reactor_datagram_socket<Derived, Service, SendToOp, RecvFromOp, DescState>::
    do_send_to(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        endpoint dest,
        std::stop_token const& token,
        std::error_code* ec,
        std::size_t* bytes_out)
{
    auto& op = wr_;
    op.reset();

    capy::mutable_buffer bufs[SendToOp::max_buffers];
    op.iovec_count =
        static_cast<int>(param.copy_to(bufs, SendToOp::max_buffers));

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

    // Set up destination address
    op.dest_len = to_sockaddr(dest, socket_family(this->fd_), op.dest_storage);
    op.fd       = this->fd_;

    // Speculative sendmsg
    msghdr msg{};
    msg.msg_name    = &op.dest_storage;
    msg.msg_namelen = op.dest_len;
    msg.msg_iov     = op.iovecs;
    msg.msg_iovlen  = static_cast<std::size_t>(op.iovec_count);

#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
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
            return dispatch_coro(ex, h);
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
        this->desc_state_.write_cancel_pending);
    return std::noop_coroutine();
}

template<
    class Derived,
    class Service,
    class SendToOp,
    class RecvFromOp,
    class DescState>
std::coroutine_handle<>
reactor_datagram_socket<Derived, Service, SendToOp, RecvFromOp, DescState>::
    do_recv_from(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param param,
        endpoint* source,
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

    // Speculative recvmsg
    msghdr msg{};
    msg.msg_name    = &op.source_storage;
    msg.msg_namelen = sizeof(op.source_storage);
    msg.msg_iov     = op.iovecs;
    msg.msg_iovlen  = static_cast<std::size_t>(op.iovec_count);

    ssize_t n;
    do
    {
        n = ::recvmsg(this->fd_, &msg, 0);
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
            if (source && !err && n > 0)
                *source = from_sockaddr(op.source_storage);
            return dispatch_coro(ex, h);
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

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_SOCKET_HPP
