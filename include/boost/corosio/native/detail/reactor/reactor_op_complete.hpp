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
template <typename Op>
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

    capy::executor_ref saved_ex(op.ex);
    std::coroutine_handle<> saved_h(op.h);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

/** Complete a connect operation with endpoint caching.

    On success, queries the local endpoint via getsockname and
    caches both endpoints in the socket impl. Then resumes the
    caller via symmetric transfer.

    @tparam Op The concrete connect operation type.
    @param op The operation to complete.
*/
template <typename Op>
void
complete_connect_op(Op& op)
{
    op.stop_cb.reset();
    op.socket_impl_->desc_state_.scheduler_->reset_inline_budget();

    bool success =
        (op.errn == 0 && !op.cancelled.load(std::memory_order_acquire));

    if (success && op.socket_impl_)
    {
        endpoint local_ep;
        sockaddr_storage local_storage{};
        socklen_t local_len = sizeof(local_storage);
        if (::getsockname(
                op.fd,
                reinterpret_cast<sockaddr*>(&local_storage),
                &local_len) == 0)
            local_ep = from_sockaddr(local_storage);
        op.socket_impl_->set_endpoints(local_ep, op.target_endpoint);
    }

    if (op.cancelled.load(std::memory_order_acquire))
        *op.ec_out = capy::error::canceled;
    else if (op.errn != 0)
        *op.ec_out = make_err(op.errn);
    else
        *op.ec_out = {};

    capy::executor_ref saved_ex(op.ex);
    std::coroutine_handle<> saved_h(op.h);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
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
template <typename SocketImpl, typename AcceptorImpl>
bool
setup_accepted_socket(
    AcceptorImpl* acceptor_impl,
    int& accepted_fd,
    sockaddr_storage const& peer_storage,
    io_object::implementation** impl_out,
    std::error_code* ec_out)
{
    auto* socket_svc = acceptor_impl->service().socket_service();
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
    socket_svc->scheduler().register_descriptor(
        accepted_fd, &impl.desc_state_);

    impl.set_endpoints(
        acceptor_impl->local_endpoint(),
        from_sockaddr(peer_storage));

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
template <typename SocketImpl, typename Op>
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
                op.acceptor_impl_,
                op.accepted_fd,
                op.peer_storage,
                op.impl_out,
                op.ec_out))
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

    capy::executor_ref saved_ex(op.ex);
    std::coroutine_handle<> saved_h(op.h);
    auto prevent = std::move(op.impl_ptr);
    dispatch_coro(saved_ex, saved_h).resume();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_OP_COMPLETE_HPP
