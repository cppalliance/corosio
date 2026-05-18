//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_ACCEPTOR_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_ACCEPTOR_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <liburing.h>

#include <boost/capy/error.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_buffer.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <netinet/in.h>
#include <sys/socket.h>

namespace boost::corosio::detail {

/** Multishot accept op — one submitted per acceptor lifetime.

    The kernel produces a CQE for each accepted connection. Each CQE
    carries the new fd in `res` (>= 0) or a negative errno on failure.
    The `IORING_CQE_F_MORE` flag is set on every CQE except the last,
    indicating whether the multishot armament is still active.

    `do_cqe` does NOT push self into `local` — the owning acceptor's
    `on_cqe` handler decides whether to dispatch immediately (waiter
    present) or park the fd (no waiter). The multishot op persists
    across CQEs; only `acceptor_impl` owns its lifetime.
*/
struct uring_multi_accept_op : io_uring_op
{
    /// Filled by the kernel for each accept. Address of this struct
    /// is registered with the SQE; kernel writes peer address here.
    sockaddr_storage  peer_storage{};
    socklen_t         peer_len    = sizeof(peer_storage);
    int               listen_fd  = -1;

    /// Owning acceptor; raw because the op IS owned by the acceptor.
    void*             acceptor_impl = nullptr;

    /** Callback into the acceptor for each accept CQE.

        @param acceptor The owning acceptor_impl pointer.
        @param new_fd   Accepted fd on success, -1 on error.
        @param err      errno value on failure, 0 on success.
        @param more     True unless this is the terminating CQE
                        (e.g. kernel dropped multishot on -ENOMEM).
    */
    void (*on_cqe)(void* acceptor, int new_fd, int err,
                   bool more) noexcept = nullptr;

    uring_multi_accept_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep)
    {}

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_multi_accept_op*>(base);
        ::io_uring_prep_multishot_accept(
            sqe, self->listen_fd,
            reinterpret_cast<sockaddr*>(&self->peer_storage),
            &self->peer_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
    }

    static void do_cqe(io_uring_op* base, int res, unsigned flags,
                       op_queue& /*local*/) noexcept
    {
        auto* self  = static_cast<uring_multi_accept_op*>(base);
        bool  more  = (flags & IORING_CQE_F_MORE) != 0;
        int   err   = (res < 0) ? -res : 0;
        int   new_fd = (res >= 0) ? res : -1;
        if (self->on_cqe)
            self->on_cqe(self->acceptor_impl, new_fd, err, more);
        // Intentionally NOT pushed into local: the acceptor decides
        // whether to surface the fd via a waiter or park it.
    }

    /// Never invoked: the multishot op is owned by the acceptor and
    /// never queued for handler dispatch. Provided so the vtable is
    /// complete.
    static void do_handler(
        void* /*owner*/, scheduler_op* /*base*/,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        // No-op. The acceptor's per-accept callback handles everything.
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
struct uring_accept_op : io_uring_op
{
    int                          accepted_fd          = -1;
    int                          err                  = 0;
    sockaddr_storage             peer_storage{};
    socklen_t                    peer_len             = 0;

    /// Set by the acceptor's `async_accept` entry point; filled by
    /// `do_handler` with the new socket impl.
    io_object::implementation**  impl_out             = nullptr;

    /// Optional output for the peer endpoint.
    endpoint*                    peer_endpoint_out    = nullptr;

    /// The peer service used to wrap the accepted fd.
    void*                        peer_service         = nullptr;

    /// Acceptor-supplied wrapper: adopts `fd` into the right impl type.
    io_object::implementation*
        (*adopt_fn)(void* peer_service, int fd,
                    sockaddr_storage const& peer,
                    socklen_t peer_len) noexcept = nullptr;

    uring_accept_op() noexcept
        : io_uring_op(&do_handler, &do_cqe)
    {}

    static void do_cqe(io_uring_op*, int, unsigned,
                       op_queue&) noexcept
    {
        // Unreachable: this op never receives a CQE.
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_accept_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            delete self;
            return;
        }

        bool was_cancelled =
            self->cancelled.load(std::memory_order_acquire);

        if (was_cancelled || self->err)
        {
            if (self->ec_out)
                *self->ec_out = was_cancelled
                    ? std::error_code(capy::error::canceled)
                    : make_err(self->err);
            self->cont_op.cont.h = self->h;
            auto next = dispatch_coro(self->ex, self->cont_op.cont);
            delete self;
            next.resume();
            return;
        }

        if (self->adopt_fn && self->impl_out)
            *self->impl_out = self->adopt_fn(
                self->peer_service, self->accepted_fd,
                self->peer_storage, self->peer_len);

        if (self->peer_endpoint_out)
            *self->peer_endpoint_out =
                sockaddr_to_endpoint(self->peer_storage);

        if (self->ec_out)
            *self->ec_out = {};

        self->cont_op.cont.h = self->h;
        auto next = dispatch_coro(self->ex, self->cont_op.cont);
        delete self;
        next.resume();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_ACCEPTOR_OPS_HPP
