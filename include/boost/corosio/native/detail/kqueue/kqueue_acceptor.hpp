//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/corosio/detail/intrusive.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_op.hpp>

#include <memory>

namespace boost::corosio::detail {

class kqueue_acceptor_service;

/// Acceptor implementation for kqueue backend.
class kqueue_acceptor final
    : public tcp_acceptor::implementation
    , public std::enable_shared_from_this<kqueue_acceptor>
    , public intrusive_list<kqueue_acceptor>::node
{
    friend class kqueue_acceptor_service;

public:
    explicit kqueue_acceptor(kqueue_acceptor_service& svc) noexcept;

    /** Initiate an asynchronous accept on the listening socket.

        Attempts a synchronous accept first. If the socket would block
        (EAGAIN), the operation is parked in desc_state_ until the
        reactor delivers a read-readiness event, at which point the
        accept is retried. On completion (success, error, or
        cancellation) the operation is posted to the scheduler and
        @a caller is resumed via @a ex.

        Only one accept may be outstanding at a time; overlapping
        calls produce undefined behavior.

        @param caller Coroutine handle resumed on completion.
        @param ex Executor through which @a caller is resumed.
        @param token Stop token for cancellation.
        @param ec Points to storage for the result error code.
        @param out_impl Points to storage for the accepted socket impl.

        @return std::noop_coroutine() unconditionally.
    */
    std::coroutine_handle<> accept(
        std::coroutine_handle<> caller,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec,
        io_object::implementation** out_impl) override;

    int native_handle() const noexcept
    {
        return fd_;
    }
    endpoint local_endpoint() const noexcept override
    {
        return local_endpoint_;
    }
    bool is_open() const noexcept override
    {
        return fd_ >= 0;
    }

    /** Cancel any pending accept operation. */
    void cancel() noexcept override;

    /** Cancel a specific pending operation.

        @param op The operation to cancel.
    */
    void cancel_single_op(kqueue_op& op) noexcept;

    /** Close the listening socket and cancel pending operations. */
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept
    {
        local_endpoint_ = ep;
    }

    kqueue_acceptor_service& service() noexcept
    {
        return svc_;
    }

private:
    kqueue_acceptor_service& svc_;
    kqueue_accept_op acc_;
    descriptor_state desc_state_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_ACCEPTOR_HPP
