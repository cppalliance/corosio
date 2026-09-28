//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/posix_descriptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/native/detail/uring/uring_file_ops.hpp>
#include <boost/corosio/native/detail/uring/uring_scheduler.hpp>
#include <boost/corosio/native/detail/uring/uring_socket_ops.hpp>
#include <boost/corosio/native/detail/validate_fd.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <system_error>

#include <errno.h>
#include <poll.h>
#include <unistd.h>

/* io_uring-backed implementation of posix_descriptor.

   Three things differ from the reactor backends and from the other
   io_uring services:

   Transfers submit READV/WRITEV at offset -1 so the kernel uses (and
   advances) the descriptor's own file position. The file services
   pass a real offset; a pipe, tty or character device has none.

   O_NONBLOCK is armed lazily, on the first read_some/write_some and
   never from assign() or wait(). Here that is a cancellability
   requirement, not only the public contract: a transfer the kernel
   would have to block on is punted to an io-wq worker, and a request
   running on a worker cannot be cancelled. With the flag set the
   kernel reports readiness instead, so every operation stays
   cancellable on every kernel version.

   That reporting is what the transfer ops' two-phase shape handles.
   An O_NONBLOCK descriptor the kernel cannot retry internally
   completes with -EAGAIN; the op then re-arms itself as a poll_add on
   the same descriptor and re-submits the transfer when the poll says
   ready. The handler makes that decision *before* coro_drain_if_shutdown,
   which disarms stop_cb: an op going round again keeps its
   cancellation wiring, so a stop_token firing between phases still
   reaches the kernel.

   The gap between a CQE and its dispatch is the whole difficulty of
   that shape, and two epoch counters close it. Nothing of a transfer
   waiting in that gap is in the ring, so neither cancel-by-fd nor a
   one-shot stop_callback can reach it; cancel() bumps cancel_epoch_
   and every descriptor change bumps desc_epoch_, and an op whose
   snapshot no longer matches completes instead of re-arming. The
   descriptor epoch is also what makes the staleness check exact
   rather than heuristic: a closed fd number the next assign() gets
   back would satisfy a bare fd comparison.

   There is no adopt-time registration. assign() validates and takes
   the descriptor; a kernel that refuses it says so at the first
   operation, as the public docstring promises.
*/

namespace boost::corosio::detail {

class uring_descriptor;

/** Advance a two-phase transfer op, or report that it is finished.

    A kernel `EAGAIN` becomes a `poll_add` on the same descriptor, and
    the poll's completion re-submits the transfer. Every path that
    stops instead leaves @a op carrying a result the completion decode
    can read as terminal.

    @param op The op whose CQE just arrived.
    @return True when a fresh SQE was submitted, in which case the
        caller must neither complete nor disarm the op.
*/
template<class Op>
bool uring_descriptor_continue(Op& op) noexcept;

/** Scatter read via `IORING_OP_READV` at the descriptor's own offset.

    @see uring_descriptor_continue for the `polling` phase.
*/
struct uring_descriptor_read_op final : uring_file_read_op_base
{
    uring_descriptor* desc = nullptr;
    /// True while the submitted SQE is the readiness poll, not the read.
    bool polling = false;
    /// Owner epochs snapshotted at submission; see uring_descriptor_continue.
    std::uint32_t cancel_epoch = 0;
    std::uint32_t desc_epoch   = 0;

    uring_descriptor_read_op() noexcept : uring_file_read_op_base(&do_handler)
    {
        prep_func = &do_prep;
    }

    static void do_prep(uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_descriptor_read_op*>(base);
        if (self->polling)
            ::io_uring_prep_poll_add(sqe, self->fd, POLLIN);
        else
            uring_file_read_op_base::do_prep(base, sqe);
    }

    static void do_handler(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error) noexcept;
};

/// Gather write via `IORING_OP_WRITEV` at the descriptor's own offset.
struct uring_descriptor_write_op final : uring_file_write_op_base
{
    uring_descriptor* desc = nullptr;
    /// True while the submitted SQE is the readiness poll, not the write.
    bool polling = false;
    /// Owner epochs snapshotted at submission; see uring_descriptor_continue.
    std::uint32_t cancel_epoch = 0;
    std::uint32_t desc_epoch   = 0;

    uring_descriptor_write_op() noexcept : uring_file_write_op_base(&do_handler)
    {
        prep_func = &do_prep;
    }

    static void do_prep(uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_descriptor_write_op*>(base);
        if (self->polling)
            ::io_uring_prep_poll_add(sqe, self->fd, POLLOUT);
        else
            uring_file_write_op_base::do_prep(base, sqe);
    }

    static void do_handler(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error) noexcept;
};

/** Native io_uring implementation of @ref posix_descriptor.

    Holds the adopted descriptor and the five embedded op slots: one
    transfer per direction, and one wait per direction.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. Each slot carries a single pending
    operation, so a descriptor must not have two operations of the
    same kind in flight.
*/
class BOOST_COROSIO_DECL uring_descriptor final
    : public posix_descriptor::implementation
    , public std::enable_shared_from_this<uring_descriptor>
{
    uring_scheduler* sched_ = nullptr;
    int fd_                 = -1;
    bool nonblocking_       = false;

    // Bumped by cancel() and by every descriptor change respectively.
    // A transfer op parked between its EAGAIN CQE and its dispatch is
    // invisible to the ring, so these are the only thing that can stop
    // it re-arming against an intent it no longer belongs to.
    std::atomic<std::uint32_t> cancel_epoch_{0};
    std::atomic<std::uint32_t> desc_epoch_{0};

    uring_descriptor_read_op rd_;
    uring_descriptor_write_op wr_;
    uring_wait_op wait_rd_;
    uring_wait_op wait_wr_;
    uring_wait_op wait_er_;

public:
    explicit uring_descriptor(uring_scheduler& sched) noexcept : sched_(&sched)
    {
    }

    ~uring_descriptor() override
    {
        close_descriptor();
    }

    // -- io_stream::implementation --

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buffers,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        rd_.prepare(
            h, ex, ec, bytes, fd_, /*file_offset=*/-1, sched_,
            shared_from_this(), buffers, token);
        arm_slot(rd_);
        sched_->work_started();

        // Closed-object contract outranks the zero-length no-op.
        if (fd_ < 0)
        {
            rd_.empty_buffer = false;
            rd_.res          = -EBADF;
            push_completed(&rd_);
            return std::noop_coroutine();
        }

        if (rd_.empty_buffer || rd_.cancelled.load(std::memory_order_acquire))
        {
            push_completed(&rd_);
            return std::noop_coroutine();
        }

        // The first transferring operation is what arms O_NONBLOCK;
        // assign() and wait() never do.
        if (int const nerr = arm_nonblocking())
        {
            rd_.res = -nerr;
            push_completed(&rd_);
            return std::noop_coroutine();
        }

        uring_submit_op(*sched_, &rd_);
        return std::noop_coroutine();
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buffers,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        wr_.prepare(
            h, ex, ec, bytes, fd_, /*file_offset=*/-1, sched_,
            shared_from_this(), buffers, token);
        arm_slot(wr_);
        sched_->work_started();

        if (fd_ < 0)
        {
            wr_.empty_buffer = false;
            wr_.res          = -EBADF;
            push_completed(&wr_);
            return std::noop_coroutine();
        }

        if (wr_.empty_buffer || wr_.cancelled.load(std::memory_order_acquire))
        {
            push_completed(&wr_);
            return std::noop_coroutine();
        }

        if (int const nerr = arm_nonblocking())
        {
            wr_.res = -nerr;
            push_completed(&wr_);
            return std::noop_coroutine();
        }

        uring_submit_op(*sched_, &wr_);
        return std::noop_coroutine();
    }

    // -- posix_descriptor::implementation --

    std::coroutine_handle<> wait(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        wait_type w,
        std::stop_token token,
        std::error_code* ec) override
    {
        uring_wait_op* op = nullptr;
        int poll_flags    = 0;
        switch (w)
        {
        case wait_type::read:
            op         = &wait_rd_;
            poll_flags = POLLIN;
            break;
        case wait_type::write:
            op         = &wait_wr_;
            poll_flags = POLLOUT;
            break;
        case wait_type::error:
            op = &wait_er_;
            // POLLERR, POLLHUP and POLLNVAL are reported whether or not
            // they are asked for, so the error wait names only POLLPRI.
            poll_flags = POLLPRI;
            break;
        }

        op->prepare(
            h, ex, ec, fd_, sched_, shared_from_this(), poll_flags, token);
        sched_->work_started();

        // No arm_nonblocking() on any branch here: a wait must leave a
        // descriptor someone else owns exactly as it found it.
        if (fd_ < 0)
        {
            op->res = -EBADF;
            push_completed(op);
            return std::noop_coroutine();
        }

        if (op->cancelled.load(std::memory_order_acquire))
        {
            push_completed(op);
            return std::noop_coroutine();
        }

        uring_submit_op(*sched_, op);
        return std::noop_coroutine();
    }

    native_handle_type native_handle() const noexcept override
    {
        return fd_;
    }

    native_handle_type release_descriptor() noexcept override
    {
        // Flush the cancel while the fd is still open so the kernel
        // resolves it before the caller can close and recycle the
        // number. Do NOT close -- the caller takes ownership.
        if (fd_ >= 0)
            sched_->cancel_and_flush(fd_);
        native_handle_type released = fd_;
        fd_                         = -1;
        nonblocking_                = false;
        desc_epoch_.fetch_add(1, std::memory_order_release);
        return released;
    }

    void cancel() noexcept override
    {
        // Bump before the SQE: cancel-by-fd reaches only what the ring
        // currently holds, and an op waiting for its handler to run
        // holds nothing there. The epoch is what that op consults.
        cancel_epoch_.fetch_add(1, std::memory_order_release);
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    /// Epoch bumped by every @ref cancel.
    std::uint32_t cancel_epoch() const noexcept
    {
        return cancel_epoch_.load(std::memory_order_acquire);
    }

    /// Epoch bumped by every change of the held descriptor.
    std::uint32_t desc_epoch() const noexcept
    {
        return desc_epoch_.load(std::memory_order_acquire);
    }

    // -- Service-facing (non-virtual) --

    /** Adopt an already-validated descriptor.

        Resets the lazy-nonblocking latch so a freshly adopted fd is
        not assumed to carry the flag from whatever this object held
        before.

        @param fd The descriptor to adopt.
    */
    void set_descriptor(int fd) noexcept
    {
        fd_          = fd;
        nonblocking_ = false;
        desc_epoch_.fetch_add(1, std::memory_order_release);
    }

    /// Cancel pending operations and close the descriptor. No-op when
    /// already closed.
    void close_descriptor() noexcept
    {
        if (fd_ < 0)
            return;
        // Both kernel entries below can run a queued pipe write as task
        // work; with the reader already gone that raises SIGPIPE.
        scoped_sigpipe_block no_sigpipe;
        sched_->cancel_and_flush(fd_);
        ::close(fd_);
        fd_          = -1;
        nonblocking_ = false;
        desc_epoch_.fetch_add(1, std::memory_order_release);
    }

private:
    /** Arm O_NONBLOCK, once, before the first transfer.

        Reports an errno rather than an error_code because the op
        result model records a negated errno in `res`; the round trip
        is lossless because fcntl only fails with codes make_err
        passes through.
    */
    int arm_nonblocking() noexcept
    {
        if (nonblocking_)
            return 0;
        if (auto ec = ensure_nonblocking(fd_))
            return ec.value();
        nonblocking_ = true;
        return 0;
    }

    /** Bind a transfer slot to this descriptor for a fresh submission.

        The epoch snapshot taken here is what a later re-arm compares
        against; see uring_descriptor_continue.
    */
    template<class Op>
    void arm_slot(Op& op) noexcept
    {
        op.desc         = this;
        op.polling      = false;
        op.cancel_epoch = cancel_epoch_.load(std::memory_order_acquire);
        op.desc_epoch   = desc_epoch_.load(std::memory_order_acquire);
    }

    /// Queue an already-counted op for the next dispatch cycle.
    void push_completed(scheduler_op* op) noexcept
    {
        uring_scheduler::lock_type lock(sched_->dispatch_mutex());
        sched_->push_completed_locked(op);
    }
};

// --- Deferred implementations (need uring_descriptor complete) ---

template<class Op>
bool
uring_descriptor_continue(Op& op) noexcept
{
    // A poll CQE carries its revents in `res` -- a small positive
    // integer the completion decode would otherwise read as a byte
    // count and as success. Every path that abandons the op while that
    // value is sitting there has to overwrite it with a terminal one.
    bool const mid_poll = op.polling && op.res >= 0;

    if (!op.desc)
    {
        if (mid_poll)
            op.res = -EBADF;
        return false;
    }

    // Two epochs rather than one, because the two reasons to abandon an
    // op name different codes to the caller.
    if (op.cancelled.load(std::memory_order_acquire) ||
        op.desc->cancel_epoch() != op.cancel_epoch)
    {
        if (mid_poll)
            op.res = -ECANCELED;
        return false;
    }

    if (op.desc->desc_epoch() != op.desc_epoch)
    {
        if (mid_poll)
            op.res = -EBADF;
        return false;
    }

    if (op.polling)
    {
        // A poll that failed or was cancelled is the operation's answer.
        if (op.res < 0)
            return false;
        op.polling = false;
    }
    else if (op.res == -EAGAIN || op.res == -EWOULDBLOCK)
    {
        op.polling = true;
    }
    else
    {
        return false;
    }

    // do_one spends a work_finished() on every op it dispatches, so an
    // op going round again has to be counted again.
    op.sched_->work_started();
    uring_submit_op(*op.sched_, &op);

    // stop_cb is one-shot and has already fired for the SQE that just
    // completed, and cancel-by-fd found nothing while this op was out
    // of the ring: a cancel racing the submission above would reach no
    // kernel request at all, so re-check and drive it here.
    if (op.cancelled.load(std::memory_order_acquire) ||
        op.desc->cancel_epoch() != op.cancel_epoch)
        op.sched_->submit_cancel_by_user_data(&op);
    return true;
}

inline void
uring_descriptor_read_op::do_handler(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/) noexcept
{
    auto* self = static_cast<uring_descriptor_read_op*>(base);
    if (owner != nullptr && uring_descriptor_continue(*self))
        return;

    if (coro_drain_if_shutdown(owner, self))
        return;

    if (self->sched_)
        self->sched_->reset_inline_budget();

    uring_set_result(self, /*is_read=*/true, self->empty_buffer);
    if (self->bytes_out)
        *self->bytes_out =
            self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;
    coro_resume(self);
}

inline void
uring_descriptor_write_op::do_handler(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/) noexcept
{
    auto* self = static_cast<uring_descriptor_write_op*>(base);
    if (owner != nullptr && uring_descriptor_continue(*self))
        return;

    if (coro_drain_if_shutdown(owner, self))
        return;

    if (self->sched_)
        self->sched_->reset_inline_budget();

    uring_set_result(self, /*is_read=*/false, self->empty_buffer);
    if (self->bytes_out)
        *self->bytes_out =
            self->res >= 0 ? static_cast<std::size_t>(self->res) : 0u;
    coro_resume(self);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_DESCRIPTOR_HPP
