//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

#include <liburing.h>

#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_buffer.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/speculative_state.hpp>

#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

namespace boost::corosio::detail {

/// Maximum scatter/gather segments per read/write/dgram op.
///
/// Bounded well below `IOV_MAX` (1024 on Linux) so each op's
/// `iovec[io_uring_max_iov]` lives inside the io_uring_op object on
/// the same allocation as the rest of its state. Plan 4's registered-
/// buffer work will revisit; until then 16 covers typical scatter use
/// cases (fragmented buffers from buffer_sequence) without bloating
/// per-op memory.
inline constexpr std::size_t io_uring_max_iov = 16;

/** Resolve ec_out/bytes_out from a CQE result for a completed I/O op.

    Shared by read, write, and connect handlers. For reads, `res == 0`
    with a non-empty buffer means the peer closed the connection (EOF).

    @param self       The completed op.
    @param is_read    True if this is a receive/read operation.
    @param empty_buf  True if the submitted buffer was zero-length.
*/
inline void
uring_set_result(io_uring_op* self, bool is_read, bool empty_buf) noexcept
{
    if (!self->ec_out)
        return;

    if (self->cancelled.load(std::memory_order_acquire))
        *self->ec_out = capy::error::canceled;
    else if (self->res < 0)
        *self->ec_out = make_err(-self->res);
    else if (is_read && self->res == 0 && !empty_buf)
        *self->ec_out = capy::error::eof;
    else
        *self->ec_out = {};
}

/** Scatter-gather read via `IORING_OP_READV`.

    @par Handler dispatch
    do_cqe captures `res`/`cqe_flags` and queues self into `local`;
    do_handler runs from the scheduler queue and resumes the coroutine.
*/
struct uring_read_op : io_uring_op
{
    iovec  iovecs[io_uring_max_iov];
    int    iovec_count = 0;
    int    fd          = -1;
    detail::speculative_state* spec_state = nullptr;

    uring_read_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep)
    {
        is_read = true;
    }

    /** Reset and initialize for a new submission.

        Embedded ops are reused across calls; every mutable field the
        handler may read must be re-initialized here. `start(token)`
        also resets `cancelled`, `sqe_set`, and `stop_cb`.

        @pre This slot has no in-flight op (its prior op completed).
    */
    void prepare(
        std::coroutine_handle<>    handle,
        capy::executor_ref         executor,
        std::error_code*           ec,
        std::size_t*               bytes,
        int                        file_descriptor,
        io_uring_scheduler*        scheduler,
        std::shared_ptr<void>      impl,
        detail::speculative_state* spec,
        buffer_param               buffers,
        std::stop_token const&     token) noexcept
    {
        h          = handle;
        ex         = executor;
        ec_out     = ec;
        bytes_out  = bytes;
        fd         = file_descriptor;
        sched_     = scheduler;
        impl_ptr   = std::move(impl);
        spec_state = spec;
        res        = 0;
        cqe_flags  = 0;
        iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        empty_buffer = (iovec_count == 0);
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_read_op*>(base);
        ::io_uring_prep_readv(
            sqe, self->fd, self->iovecs, self->iovec_count, 0);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_read_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_read_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            // Shutdown drain: break the impl_ptr cycle. The op storage
            // is owned by the impl, which destructs once the cycle is
            // broken (if this was the last ref).
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        uring_set_result(self, true, self->empty_buffer);

        if (self->res > 0 && self->spec_state)
        {
            // Kernel signalled readiness — restore speculation.
            self->spec_state->on_async_read_ready();
        }

        if (self->bytes_out)
            *self->bytes_out =
                self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;

        self->cont_op.cont.h = self->h;
        auto next = dispatch_coro(self->ex, self->cont_op.cont);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
        // suicide drops here; may destroy impl + self.
    }
};

/** Scatter-gather write via `IORING_OP_SENDMSG` with `MSG_NOSIGNAL`.

    `MSG_NOSIGNAL` prevents `SIGPIPE` when the peer has closed the
    connection; the error is surfaced as `EPIPE` instead.
*/
struct uring_write_op : io_uring_op
{
    iovec  iovecs[io_uring_max_iov];
    int    iovec_count = 0;
    int    fd          = -1;
    msghdr msg{};
    detail::speculative_state* spec_state = nullptr;

    uring_write_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep)
    {}

    /** Reset and initialize for a new submission. See uring_read_op::prepare. */
    void prepare(
        std::coroutine_handle<>    handle,
        capy::executor_ref         executor,
        std::error_code*           ec,
        std::size_t*               bytes,
        int                        file_descriptor,
        io_uring_scheduler*        scheduler,
        std::shared_ptr<void>      impl,
        detail::speculative_state* spec,
        buffer_param               buffers,
        std::stop_token const&     token) noexcept
    {
        h          = handle;
        ex         = executor;
        ec_out     = ec;
        bytes_out  = bytes;
        fd         = file_descriptor;
        sched_     = scheduler;
        impl_ptr   = std::move(impl);
        spec_state = spec;
        res        = 0;
        cqe_flags  = 0;
        iovec_count = static_cast<int>(
            buffers.copy_to(
                reinterpret_cast<capy::mutable_buffer*>(iovecs),
                io_uring_max_iov));
        empty_buffer = (iovec_count == 0);
        if (!empty_buffer)
        {
            msg = {};
            msg.msg_iov    = iovecs;
            msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iovec_count);
        }
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_write_op*>(base);
        ::io_uring_prep_sendmsg(
            sqe, self->fd, &self->msg, MSG_NOSIGNAL);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_write_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_write_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        uring_set_result(self, false, self->empty_buffer);

        if (self->res > 0 && self->spec_state)
        {
            // Kernel signalled readiness — restore speculation.
            self->spec_state->on_async_write_ready();
        }

        if (self->bytes_out)
            *self->bytes_out =
                self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;

        self->cont_op.cont.h = self->h;
        auto next = dispatch_coro(self->ex, self->cont_op.cont);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
    }
};

/** Non-blocking connect via `IORING_OP_CONNECT`.

    Negative `res` is the connect error; zero means success.
    `remote_endpoint_out` is written only on success so a failed
    connect does not corrupt the socket's cached remote endpoint.
*/
struct uring_connect_op : io_uring_op
{
    sockaddr_storage addr{};
    socklen_t        addrlen            = 0;
    int              fd                 = -1;
    endpoint         target_endpoint{};
    endpoint*        remote_endpoint_out = nullptr;
    endpoint*        local_endpoint_out  = nullptr;

    uring_connect_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep)
    {}

    /** Reset and initialize for a new submission.

        The caller must fill `addr` and `addrlen` before calling this
        (typically via `to_sockaddr(ep, family, conn_.addr)` which
        returns the addrlen) — `to_sockaddr` is the family-aware
        helper and requires the socket family which is known to the
        caller, not the op.
    */
    void prepare(
        std::coroutine_handle<>  handle,
        capy::executor_ref       executor,
        std::error_code*         ec,
        int                      file_descriptor,
        io_uring_scheduler*      scheduler,
        std::shared_ptr<void>    impl,
        endpoint                 target,
        endpoint*                remote_out,
        endpoint*                local_out,
        std::stop_token const&   token) noexcept
    {
        h         = handle;
        ex        = executor;
        ec_out    = ec;
        bytes_out = nullptr;
        fd        = file_descriptor;
        sched_    = scheduler;
        impl_ptr  = std::move(impl);
        res       = 0;
        cqe_flags = 0;
        target_endpoint     = target;
        remote_endpoint_out = remote_out;
        local_endpoint_out  = local_out;
        // addr / addrlen are pre-filled by the caller.
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_connect_op*>(base);
        ::io_uring_prep_connect(
            sqe, self->fd,
            reinterpret_cast<sockaddr const*>(&self->addr),
            self->addrlen);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_connect_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_connect_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        uring_set_result(self, false, false);

        // Write endpoints only on success.
        if (self->res >= 0)
        {
            if (self->remote_endpoint_out)
                *self->remote_endpoint_out = self->target_endpoint;
            if (self->local_endpoint_out && self->fd >= 0)
            {
                sockaddr_storage local{};
                socklen_t len = sizeof(local);
                if (::getsockname(self->fd,
                        reinterpret_cast<sockaddr*>(&local), &len) == 0)
                    *self->local_endpoint_out = sockaddr_to_endpoint(local);
            }
        }

        self->cont_op.cont.h = self->h;
        auto next = dispatch_coro(self->ex, self->cont_op.cont);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
    }
};

/** Submit an `io_uring_op` whose `prep_func` is set.

    Acquires the ring mutex, prepares the SQE, and (under the same
    mutex) CAS-sets `submit_op_posted_`. The first submitter of a
    batch wins the CAS and posts the scheduler's `submit_sqes_op`,
    which later flushes all queued SQEs in a single
    `io_uring_submit_and_get_events` call and drains any ready CQEs.
    Subsequent submitters in the same batch piggyback — their SQEs
    sit in the user-space SQ ring until that op dispatches.

    On SQ-ring exhaustion (after one flush retry), surfaces `EAGAIN`
    on `*op->ec_out` and queues the op as completed so its handler
    dispatches on the next `do_one` cycle.

    @pre `op->prep_func != nullptr`.

    @par Exception Safety
    Nothrow.
*/
inline void
io_uring_submit_op(io_uring_scheduler& sched, io_uring_op* op) noexcept
{
    sched.lazy_init_ring();

    bool need_post = false;
    {
        typename io_uring_scheduler::lock_type ring_lock(sched.ring_mutex());

        ::io_uring_sqe* sqe = ::io_uring_get_sqe(sched.ring());
        if (!sqe)
        {
            // SQ ring full — flush to kernel and retry once.
            ::io_uring_submit(sched.ring());
            sqe = ::io_uring_get_sqe(sched.ring());
        }

        if (!sqe)
        {
            // SQ stayed full after one flush — synchronous failure path.
            // Surface EAGAIN and queue the op as completed so do_one
            // dispatches the handler. The caller's work_started() already
            // counted this op. (CAS path is not entered here.)
            if (op->ec_out)
                *op->ec_out = make_err(EAGAIN);
            typename io_uring_scheduler::lock_type lock(sched.dispatch_mutex());
            sched.push_completed_locked(op);
            return;
        }

        op->prep_func(op, sqe);
        ::io_uring_sqe_set_data(sqe, op);
        // Release pairs with the acquire in io_uring_op::request_cancel:
        // a stop_token firing after we release the mutex will see
        // sqe_set==true and submit a cancel-by-user_data SQE.
        op->sqe_set.store(true, std::memory_order_release);

        // First submitter in a batch wins the CAS and will post
        // submit_sqes_op; others piggyback on the same flush.
        if (!sched.submit_op_posted_exchange(true))
            need_post = true;
    }

    if (need_post)
    {
        // Flush is deferred to submit_sqes_op; post() owns the wake.
        sched.post(&sched.submit_op_ref());
    }
}

/** Non-blocking connect for Unix domain sockets via `IORING_OP_CONNECT`.

    Like `uring_connect_op` but stores `local_endpoint` for the target
    and out-pointers, since `sockaddr_to_local_endpoint` returns
    `local_endpoint`, not `endpoint`.
*/
struct uring_local_connect_op : io_uring_op
{
    sockaddr_storage  addr{};
    socklen_t         addrlen             = 0;
    int               fd                  = -1;
    corosio::local_endpoint    target_endpoint{};
    corosio::local_endpoint*   remote_endpoint_out = nullptr;
    corosio::local_endpoint*   local_endpoint_out  = nullptr;

    uring_local_connect_op() noexcept
        : io_uring_op(&do_handler, &do_cqe, &do_prep)
    {}

    /** Reset and initialize for a new submission.

        Caller pre-fills `addr` and `addrlen` (see uring_connect_op::prepare).
    */
    void prepare(
        std::coroutine_handle<>          handle,
        capy::executor_ref               executor,
        std::error_code*                 ec,
        int                              file_descriptor,
        io_uring_scheduler*              scheduler,
        std::shared_ptr<void>            impl,
        corosio::local_endpoint          target,
        corosio::local_endpoint*         remote_out,
        corosio::local_endpoint*         local_out,
        std::stop_token const&           token) noexcept
    {
        h         = handle;
        ex        = executor;
        ec_out    = ec;
        bytes_out = nullptr;
        fd        = file_descriptor;
        sched_    = scheduler;
        impl_ptr  = std::move(impl);
        res       = 0;
        cqe_flags = 0;
        target_endpoint     = target;
        remote_endpoint_out = remote_out;
        local_endpoint_out  = local_out;
        start(token);
    }

    static void do_prep(io_uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_local_connect_op*>(base);
        ::io_uring_prep_connect(
            sqe, self->fd,
            reinterpret_cast<sockaddr const*>(&self->addr),
            self->addrlen);
    }

    static void do_cqe(
        io_uring_op* base, int res, unsigned flags,
        op_queue& local) noexcept
    {
        auto* self      = static_cast<uring_local_connect_op*>(base);
        self->res       = res;
        self->cqe_flags = flags;
        local.push(self);
    }

    static void do_handler(
        void* owner, scheduler_op* base,
        std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
    {
        auto* self = static_cast<uring_local_connect_op*>(base);
        self->stop_cb.reset();

        if (owner == nullptr)
        {
            auto suicide = std::move(self->impl_ptr);
            return;
        }

        uring_set_result(self, false, false);

        // Write endpoints only on success.
        if (self->res >= 0)
        {
            if (self->remote_endpoint_out)
                *self->remote_endpoint_out = self->target_endpoint;
            if (self->local_endpoint_out && self->fd >= 0)
            {
                sockaddr_storage local{};
                socklen_t len = sizeof(local);
                if (::getsockname(self->fd,
                        reinterpret_cast<sockaddr*>(&local), &len) == 0)
                    *self->local_endpoint_out =
                        sockaddr_to_local_endpoint(local, len);
            }
        }

        self->cont_op.cont.h = self->h;
        auto next = dispatch_coro(self->ex, self->cont_op.cont);
        auto suicide = std::move(self->impl_ptr);
        next.resume();
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SOCKET_OPS_HPP
