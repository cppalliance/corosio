//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/kqueue/op.hpp"
#include "src/detail/kqueue/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    kqueue acceptor components:

    kqueue_acceptor_impl   – per-listener state; owns the listening fd,
                             a descriptor_state for edge-triggered readiness,
                             and a single kqueue_accept_op slot (acc_).
                             Inherits enable_shared_from_this so pending ops
                             can prevent premature destruction.

    kqueue_acceptor_state  – shared state for the service: an intrusive list
                             of live acceptor impls, a shared_ptr map for
                             ownership, and a mutex guarding both.

    kqueue_acceptor_service – execution_context service keyed by
                              acceptor_service (base class). Creates/destroys
                              impls, opens listening sockets, and forwards
                              post/work_started/work_finished to the scheduler.
                              Shutdown walks the impl list and closes all fds.
*/

namespace boost::corosio::detail {

class kqueue_acceptor_service;
class kqueue_acceptor_impl;
class kqueue_socket_service;

/// Acceptor implementation for kqueue backend.
class kqueue_acceptor_impl
    : public tcp_acceptor::acceptor_impl
    , public std::enable_shared_from_this<kqueue_acceptor_impl>
    , public intrusive_list<kqueue_acceptor_impl>::node
{
    friend class kqueue_acceptor_service;

public:
    explicit kqueue_acceptor_impl(kqueue_acceptor_service& svc) noexcept;

    /** Initiate an asynchronous accept on the listening socket.

        Attempts a synchronous accept first. If the socket would block
        (EAGAIN), the operation is parked in desc_state_ until the
        reactor delivers a read-readiness event, at which point the
        accept is retried. On completion (success, error, or
        cancellation) the operation is posted to the scheduler and
        @a caller is resumed via @a ex.

        Only one accept may be outstanding at a time; overlapping
        calls produce undefined behavior.

        @param caller Coroutine handle resumed on completion. The
            caller must remain valid until completion.
        @param ex Executor through which @a caller is resumed.
        @param token Stop token for cancellation. When stop is
            requested, the pending op completes with
            capy::error::canceled. Cancellation is asynchronous;
            the op may complete with success if the accept races
            ahead of the stop request.
        @param ec Points to storage for the result error code.
            Must remain valid until the completion handler runs.
            Set to {} on success, capy::error::canceled on
            cancellation, or a POSIX errno mapping on failure.
        @param out_impl Points to storage for the accepted socket
            impl. Must remain valid until the completion handler
            runs. Set to the new socket impl on success, nullptr
            on error or cancellation.

        @return std::noop_coroutine() unconditionally; the caller
            is always resumed asynchronously via the scheduler.

        @par Example
        @code
        std::error_code ec;
        io_object::io_object_impl* peer = nullptr;
        co_await acceptor_impl.accept(
            my_handle, ex, stop_source.get_token(), &ec, &peer);
        if (!ec)
            // peer is a valid kqueue_socket_impl
        @endcode
    */
    std::coroutine_handle<> accept(
        std::coroutine_handle<> caller,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec,
        io_object::io_object_impl** out_impl) override;

    int native_handle() const noexcept { return fd_; }
    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    bool is_open() const noexcept override { return fd_ >= 0; }

    /** Cancel any pending accept operation.

        If an accept is parked in desc_state_, it is extracted
        under the descriptor mutex, posted to the scheduler, and
        will complete with capy::error::canceled.

        Safe to call from any thread. If no operation is pending,
        this is a no-op.
    */
    void cancel() noexcept override;

    /** Cancel a specific pending operation.

        Called from the stop_token callback when cancellation is
        requested during the window between parking the op and
        the reactor delivering an event. Extracts @a op from
        desc_state_ under the descriptor mutex if it matches.

        Safe to call concurrently with the reactor thread.

        @param op The operation to cancel.
    */
    void cancel_single_op(kqueue_op& op) noexcept;

    /** Close the listening socket and cancel pending operations.

        Calls cancel(), deregisters the fd from kqueue, closes
        the fd, and resets descriptor state. If the descriptor_state
        is enqueued in the scheduler's ready queue, the impl is
        prevented from destruction via shared_from_this() until
        the queued entry is processed.

        Safe to call from any thread. After return, is_open()
        returns false.
    */
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept { local_endpoint_ = ep; }

    kqueue_acceptor_service& service() noexcept { return svc_; }

private:
    kqueue_acceptor_service& svc_;
    kqueue_accept_op acc_;
    descriptor_state desc_state_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

/** State for kqueue acceptor service. */
class kqueue_acceptor_state
{
    friend class kqueue_acceptor_service;

public:
    explicit kqueue_acceptor_state(kqueue_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

private:
    kqueue_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<kqueue_acceptor_impl> acceptor_list_;
    std::unordered_map<kqueue_acceptor_impl*, std::shared_ptr<kqueue_acceptor_impl>> acceptor_ptrs_;
};

/** kqueue acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class kqueue_acceptor_service : public acceptor_service
{
public:
    explicit kqueue_acceptor_service(capy::execution_context& ctx);
    ~kqueue_acceptor_service();

    kqueue_acceptor_service(kqueue_acceptor_service const&) = delete;
    kqueue_acceptor_service& operator=(kqueue_acceptor_service const&) = delete;

    /** Synchronously close all acceptor fds and cancel pending ops.
        Idempotent; called by the execution_context during teardown.
    */
    void shutdown() override;

    /// Construct a new acceptor impl owned by this service.
    io_object::io_object_impl* construct() override;

    /// Destroy an impl previously returned by construct().
    void destroy(io_object::io_object_impl*) override;

    /// Open the acceptor (no-op; opening is done by open_acceptor).
    void open(io_object::handle&) override;

    /// Close the acceptor's listening socket.
    void close(io_object::handle&) override;

    /** Bind and listen on @p ep with the given @p backlog.
        Registers the fd with kqueue on success and caches the
        local endpoint. Returns a non-zero std::error_code on
        any syscall failure (socket, bind, listen, fcntl).
    */
    std::error_code open_acceptor(
        tcp_acceptor::acceptor_impl& impl,
        endpoint ep,
        int backlog) override;

    kqueue_scheduler& scheduler() const noexcept { return state_->sched_; }

    /** Post a completed operation to the scheduler for execution.

        Forwards @a op to the scheduler's completion queue. The
        scheduler takes ownership; the caller must not destroy
        @a op after this call.

        @param op Operation to enqueue. Must not be null.
    */
    void post(kqueue_op* op);

    /** Increment the scheduler's outstanding work count.

        Must be paired with a subsequent call to work_finished().
        Keeps the scheduler's run() loop alive while the operation
        is in flight. Thread-safe.
    */
    void work_started() noexcept;

    /** Decrement the scheduler's outstanding work count.

        Must be paired with a prior call to work_started(). When
        the count reaches zero, the scheduler may stop. Thread-safe.
    */
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    kqueue_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<kqueue_acceptor_state> state_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP
