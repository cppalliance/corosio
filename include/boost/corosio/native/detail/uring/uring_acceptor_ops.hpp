//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_ACCEPTOR_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_ACCEPTOR_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <liburing.h>

#include <boost/capy/error.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/uring/uring_buffer.hpp>
#include <boost/corosio/native/detail/uring/uring_op.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** Multishot accept op: one kernel arming of an acceptor's listener.

    The kernel produces a CQE for each accepted connection, carrying
    the new fd in `res` (>= 0) or a negative errno. Every CQE but the
    last sets `IORING_CQE_F_MORE`; the last ends the arming.

    While armed, the op is owned by the kernel: it holds a reference on
    its acceptor (`object_ref_`) and the acceptor only remembers it by
    address. Deliveries are routed inline by `on_cqe` under the ring
    mutex. The terminal CQE queues the op for dispatch, and its handler
    (`on_end`, off every lock) either re-arms it or returns it to the
    acceptor's free list and drops the reference.
*/
struct uring_multi_accept_op
    : uring_op
    , intrusive_list<uring_multi_accept_op>::node
{
    /// Filled by the kernel for each accept. Address of this struct
    /// is registered with the SQE; kernel writes peer address here.
    sockaddr_storage peer_storage{};
    socklen_t peer_len = sizeof(peer_storage);
    int listen_fd      = -1;

    /// The acceptor that armed this op.
    void* acceptor_impl = nullptr;

    /** Route one accept CQE; runs under the ring mutex.

        @param acceptor The acceptor that armed @p op.
        @param op       The arming the CQE belongs to.
        @param new_fd   Accepted fd on success, -1 on error.
        @param err      errno value on failure, 0 on success.
        @param more     True unless this CQE ends the arming.
    */
    void (*on_cqe)(
        void* acceptor,
        uring_multi_accept_op* op,
        int new_fd,
        int err,
        bool more) noexcept = nullptr;

    /** Handle the end of an arming; runs from dispatch, off every lock.

        @param acceptor The acceptor that armed @p op.
        @param op       The ended arming.
        @param live     False when the scheduler is shutting down.
    */
    void (*on_end)(
        void* acceptor,
        uring_multi_accept_op* op,
        bool live) noexcept = nullptr;

    uring_multi_accept_op() noexcept : uring_op(&do_handler, &do_cqe, &do_prep)
    {
    }

    static void do_prep(uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_multi_accept_op*>(base);
        ::io_uring_prep_multishot_accept(
            sqe, self->listen_fd,
            reinterpret_cast<sockaddr*>(&self->peer_storage), &self->peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
    }

    static void
    do_cqe(uring_op* base, int res, unsigned flags, ready_queue& local) noexcept
    {
        auto* self = static_cast<uring_multi_accept_op*>(base);
        bool more  = (flags & IORING_CQE_F_MORE) != 0;
        int err    = (res < 0) ? -res : 0;
        int new_fd = (res >= 0) ? res : -1;
        self->on_cqe(self->acceptor_impl, self, new_fd, err, more);
        if (!more)
        {
            // Balance the work_finished() do_one runs after dispatching.
            self->sched_->work_started();
            local.push(self);
        }
    }

    static void do_handler(
        void* owner,
        scheduler_op* base,
        std::uint32_t /*bytes*/,
        std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_multi_accept_op*>(base);
        self->on_end(self->acceptor_impl, self, owner != nullptr);
    }
};

/** Synthesized accept op — manufactured by the acceptor for parked fds.

    When `async_accept` arrives and a ready fd is already parked, the
    acceptor builds one of these, fills `accepted_fd` and peer storage
    from the parked node, and posts it to the scheduler. This op never
    interacts with the ring directly — it goes straight to handler
    dispatch via `(*op)()`.

    `do_cqe` is unused (this op never receives a kernel CQE).
*/
struct uring_accept_op : uring_op
{
    int accepted_fd = -1;
    int err         = 0;
    sockaddr_storage peer_storage{};
    socklen_t peer_len = 0;

    /// Set by the acceptor's `async_accept` entry point; filled by
    /// `do_handler` with the new socket impl.
    io_object::implementation** impl_out = nullptr;

    /// Optional output for the peer endpoint.
    endpoint* peer_endpoint_out = nullptr;

    /// The peer service used to wrap the accepted fd.
    void* peer_service = nullptr;

    /// Acceptor-supplied wrapper: adopts `fd` into the right impl type.
    io_object::implementation* (*adopt_fn)(
        void* peer_service,
        int fd,
        sockaddr_storage const& peer,
        socklen_t peer_len) noexcept = nullptr;

    /** Disposal hook, run exactly once when the op is consumed.

        Heap-allocated ops delete themselves (the default); the
        multishot acceptor's pooled nodes override this to return to
        their acceptor's free list instead. Factoring disposal out of
        `do_handler` is what lets one completion routine serve both
        ownership models.
    */
    void (*dispose)(uring_accept_op*) noexcept = &dispose_delete;

    uring_accept_op() noexcept : uring_op(&do_handler, &do_cqe) {}

    static void dispose_delete(uring_accept_op* op) noexcept
    {
        delete op;
    }

    // LCOV_EXCL_START: never receives a CQE; present for vtable
    // completeness.
    static void do_cqe(uring_op*, int, unsigned, ready_queue&) noexcept {}
    // LCOV_EXCL_STOP

    static void do_handler(
        void* owner,
        scheduler_op* base,
        std::uint32_t /*bytes*/,
        std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_accept_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            // A delivered connection that nobody will adopt.
            if (self->accepted_fd >= 0)
                ::close(self->accepted_fd);
            self->dispose(self);
            return;
        }

        bool was_cancelled = self->cancelled.load(std::memory_order_acquire);

        if (was_cancelled || self->err)
        {
            if (self->ec_out)
                *self->ec_out = was_cancelled
                    ? std::error_code(capy::error::canceled)
                    : make_err(self->err);
            auto next = dispatch_coro(self->ex, *self->cont);
            self->dispose(self);
            next.resume();
            return;
        }

        if (self->adopt_fn && self->impl_out)
            *self->impl_out = self->adopt_fn(
                self->peer_service, self->accepted_fd, self->peer_storage,
                self->peer_len);

        // LCOV_EXCL_START: no public accept overload reports the peer
        // endpoint on this backend yet.
        if (self->peer_endpoint_out)
            *self->peer_endpoint_out = sockaddr_to_endpoint(self->peer_storage);
        // LCOV_EXCL_STOP

        if (self->ec_out)
            *self->ec_out = {};

        auto next = dispatch_coro(self->ex, *self->cont);
        self->dispose(self);
        next.resume();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_ACCEPTOR_OPS_HPP
