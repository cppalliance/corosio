//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_COMPLETE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_COMPLETE_HPP

#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/io/io_object.hpp>

#include <coroutine>
#include <mutex>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** Complete a base read/write operation.

    Translates the recorded errno and cancellation state into
    an error_code, stores the byte count, then resumes the
    caller via symmetric transfer.

    @tparam Op The concrete operation type.
    @param op The operation to complete.
*/
template<typename Op>
void
complete_io_op(Op& op)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else if (op.is_read_operation() && op.bytes_transferred == 0)
        *op.ec_out = capy::error::eof;
    else
        *op.ec_out = {};

    *op.bytes_out = op.bytes_transferred;

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Complete a datagram recv operation (connected mode).

    Like complete_io_op but does not translate zero bytes into
    EOF. Zero-length datagrams are valid and should be reported
    as success with 0 bytes transferred.

    @param op The operation to complete.
*/
template<typename Op>
void
complete_dgram_recv_op(Op& op)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    *op.bytes_out = op.bytes_transferred;

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Complete a wait operation.

    Wait operations report only an error_code — no bytes_transferred,
    no EOF translation. Used for socket and acceptor wait() awaitables;
    picks the impl pointer set by start() to reach the scheduler.

    @tparam Op The concrete wait operation type.
    @param op The operation to complete.
*/
template<typename Op>
void
complete_wait_op(Op& op)
{
    op.stop_cb.reset();
    if (op.socket_impl_)
        op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();
    else
        op.acceptor_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Complete a connect operation with endpoint caching.

    On success, queries the local endpoint via getsockname and
    caches both endpoints in the socket impl. Then resumes the
    caller via symmetric transfer.

    @tparam Op The concrete connect operation type.
    @param op The operation to complete.
*/
template<typename Op>
void
complete_connect_op(Op& op)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    bool success =
        (op.errn == 0 && !op.cancelled.load(std::memory_order_acquire));

    if (success && op.socket_impl_)
    {
        using ep_type = decltype(op.target_endpoint);
        ep_type local_ep;
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                op.fd, reinterpret_cast<sockaddr*>(&local_storage),
                &local_len) == 0)
            local_ep =
                from_sockaddr_as(local_storage, local_len, ep_type{});
        op.socket_impl_->set_endpoints(local_ep, op.target_endpoint);
    }

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Construct and register a peer socket from an accepted fd.

    Creates a new socket impl via the acceptor's associated
    socket service, registers it with the scheduler, and caches
    the local and remote endpoints.

    @tparam SocketImpl The concrete socket implementation type.
    @tparam AcceptorImpl The concrete acceptor implementation type.
    @param acceptor_impl The acceptor that accepted the connection.
    @param accepted_fd The accepted file descriptor (set to -1 on success).
    @param peer_storage The peer address from accept().
    @param impl_out Output pointer for the new socket impl.
    @param ec_out Output pointer for any error.
    @return True on success, false on failure.
*/
template<typename SocketImpl, typename AcceptorImpl>
bool
setup_accepted_socket(
    AcceptorImpl* acceptor_impl,
    int& accepted_fd,
    sockaddr_storage const& peer_storage,
    socklen_t peer_addrlen,
    io_object::implementation** impl_out,
    std::error_code* ec_out)
{
    auto* socket_svc = acceptor_impl->service().stream_service();
    if (!socket_svc)
    {
        *ec_out = make_err(ENOENT);
        return false;
    }

    auto& impl = static_cast<SocketImpl&>(*socket_svc->construct());
    impl.set_socket(accepted_fd);

    impl.desc_state_.fd = accepted_fd;
    {
        std::lock_guard lock(impl.desc_state_.mutex);
        impl.desc_state_.read_op    = nullptr;
        impl.desc_state_.write_op   = nullptr;
        impl.desc_state_.connect_op = nullptr;
    }
    socket_svc->scheduler().register_descriptor(accepted_fd, &impl.desc_state_);

    using ep_type = decltype(acceptor_impl->local_endpoint());
    impl.set_endpoints(
        acceptor_impl->local_endpoint(),
        from_sockaddr_as(
            peer_storage,
            peer_addrlen,
            ep_type{}));

    if (impl_out)
        *impl_out = &impl;
    accepted_fd = -1;
    return true;
}

/** Complete an accept operation.

    Sets up the peer socket on success, or closes the accepted
    fd on failure. Then resumes the caller via symmetric transfer.

    @tparam SocketImpl The concrete socket implementation type.
    @tparam Op The concrete accept operation type.
    @param op The operation to complete.
*/
template<typename SocketImpl, typename Op>
void
complete_accept_op(Op& op)
{
    op.stop_cb.reset();
    op.acceptor_impl_->desc_state_.scheduler_->reset_inline_budget();

    bool success =
        (op.errn == 0 && !op.cancelled.load(std::memory_order_acquire));

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    if (success && op.accepted_fd >= 0 && op.acceptor_impl_)
    {
        if (!setup_accepted_socket<SocketImpl>(
                op.acceptor_impl_, op.accepted_fd, op.peer_storage,
                op.peer_addrlen, op.impl_out, op.ec_out))
            success = false;
    }

    if (!success || !op.acceptor_impl_)
    {
        if (op.accepted_fd >= 0)
        {
            ::close(op.accepted_fd);
            op.accepted_fd = -1;
        }
        if (op.impl_out)
            *op.impl_out = nullptr;
    }

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Complete a datagram operation (send_to or recv_from).

    For recv_from operations, writes the source endpoint from the
    recorded sockaddr_storage into the caller's endpoint pointer.
    Then resumes the caller via symmetric transfer.

    @tparam Op The concrete datagram operation type.
    @param op The operation to complete.
*/
template<typename Op>
void
complete_datagram_op(Op& op)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    *op.bytes_out = op.bytes_transferred;

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

/** Complete a datagram operation with source endpoint capture.

    For recv_from operations, writes the source endpoint from the
    recorded sockaddr_storage into the caller's endpoint pointer.
    Then resumes the caller via symmetric transfer.

    @tparam Op The concrete datagram operation type.
    @param op The operation to complete.
    @param source_out Optional pointer to store source endpoint
        (non-null for recv_from, null for send_to).
*/
template<typename Op, typename Endpoint>
void
complete_datagram_op(Op& op, Endpoint* source_out)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    *op.bytes_out = op.bytes_transferred;

    if (source_out && !op.cancelled.load(std::memory_order_acquire) &&
        op.errn == 0)
        *source_out = from_sockaddr_as(
            op.source_storage,
            op.source_addrlen,
            Endpoint{});

    op.cont_op.cont.h = op.h;
    capy::executor_ref saved_ex(op.ex);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, op.cont_op.cont).resume();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_COMPLETE_HPP
