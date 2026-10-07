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

#include <boost/corosio/posix_stream_descriptor.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/detail/intrusive.hpp>
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
#include <sys/epoll.h>
#include <unistd.h>

/* io_uring-backed implementation of posix_stream_descriptor.

   Three things differ from the reactor backends and from the other
   io_uring services:

   Transfers submit READV/WRITEV at offset -1 so the kernel uses (and
   advances) the descriptor's own file position. The file services
   pass a real offset; a pipe, tty or character device has none.

   The descriptor's flags are never modified, as in asio. A transfer
   on a blocking fd the kernel can poll parks in its internal poll and
   ASYNC_CANCEL removes it. One the kernel punts to an io-wq worker
   (no poll support, or no FMODE_NOWAIT) holds that worker; the cancel
   interrupts it only if the driver's wait is interruptible, and the
   resulting -EINTR is reported as canceled. A caller who made the fd
   non-blocking gets EAGAIN completions, which the two-phase shape
   below turns into a poll and a retry. A file with no poll support
   is ready to every poll, so assign probes for it (fd_is_pollable)
   and its EAGAIN fails with EOPNOTSUPP instead, as on the reactors.

   An O_NONBLOCK descriptor the kernel cannot retry internally
   completes with -EAGAIN; the op then re-arms itself as a poll_add on
   the same descriptor and re-submits the transfer when the poll says
   ready. The handler makes that decision *before* coro_drain_if_shutdown,
   which disarms stop_cb: an op going round again keeps its
   cancellation wiring, so a stop_token firing between phases still
   reaches the kernel.

   The gap between a CQE and its dispatch is the whole difficulty of
   that shape, and one epoch counter closes it. Nothing of a transfer
   waiting in that gap is in the ring, so cancel-by-fd cannot reach
   it. cancel() and every descriptor change bump epoch_ before they
   take ring_mutex_, and do_prep, which runs under ring_mutex_,
   compares the op's snapshot against it: an op that no longer
   matches preps a NOP instead of a transfer and completes canceled.
   Either the prep sees the bump, or the cancel's SQE queues behind
   the transfer's and finds it. The epoch is also what makes the
   staleness check exact rather than heuristic: a closed fd number
   the next assign() gets back would satisfy a bare fd comparison.

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

/// True when @a op no longer belongs to its owner's current intent.
/// Called from do_prep, under ring_mutex_.
template<class Op>
bool uring_descriptor_abandoned(Op const& op) noexcept;

/** Scatter read via `IORING_OP_READV` at the descriptor's own offset.

    @see uring_descriptor_continue for the `polling` phase.
*/
struct uring_descriptor_read_op final : uring_file_read_op_base
{
    uring_descriptor* desc = nullptr;
    /// True while the submitted SQE is the readiness poll, not the read.
    bool polling = false;
    /// Owner epoch snapshotted by arm_slot; see uring_descriptor_continue.
    std::uint32_t epoch = 0;
    /// Set by do_prep when the op was abandoned before its SQE was built.
    bool abandoned = false;

    uring_descriptor_read_op() noexcept : uring_file_read_op_base(&do_handler)
    {
        prep_func = &do_prep;
    }

    static void do_prep(uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_descriptor_read_op*>(base);
        // Decided here because every cancel path records its intent
        // before taking ring_mutex_, which this runs under: either the
        // intent is visible now, or the cancel SQE queues behind ours.
        if (uring_descriptor_abandoned(*self))
        {
            self->abandoned = true;
            ::io_uring_prep_nop(sqe);
            return;
        }
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
    /// Owner epoch snapshotted by arm_slot; see uring_descriptor_continue.
    std::uint32_t epoch = 0;
    /// Set by do_prep when the op was abandoned before its SQE was built.
    bool abandoned = false;

    uring_descriptor_write_op() noexcept : uring_file_write_op_base(&do_handler)
    {
        prep_func = &do_prep;
    }

    static void do_prep(uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        auto* self = static_cast<uring_descriptor_write_op*>(base);
        // Decided here because every cancel path records its intent
        // before taking ring_mutex_, which this runs under: either the
        // intent is visible now, or the cancel SQE queues behind ours.
        if (uring_descriptor_abandoned(*self))
        {
            self->abandoned = true;
            ::io_uring_prep_nop(sqe);
            return;
        }
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

/** Native io_uring implementation of @ref posix_stream_descriptor.

    Holds the adopted descriptor and the five embedded op slots: one
    transfer per direction, and one wait per direction.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. Each slot carries a single pending
    operation, so a descriptor must not have two operations of the
    same kind in flight.
*/
/** Return whether the kernel can poll @p fd.

    epoll refuses a file with no poll support with EPERM; io_uring
    instead reports such a file ready to every poll. Nothing is left
    registered and @p fd is not modified. When the probe itself fails,
    the descriptor is assumed pollable.
*/
inline bool
fd_is_pollable(int fd) noexcept
{
    int ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return true;
    ::epoll_event ev{};
    bool const pollable =
        ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0 || errno != EPERM;
    ::close(ep);
    return pollable;
}

class uring_descriptor_service;

class BOOST_COROSIO_DECL uring_descriptor final
    : public posix_stream_descriptor::implementation
    , public intrusive_list<uring_descriptor>::node
{
    friend class uring_descriptor_service;

    uring_descriptor_service* svc_ = nullptr;
    uring_scheduler* sched_        = nullptr;
    int fd_                 = -1;
    bool pollable_          = true;

    // Bumped by cancel() and by every descriptor change. A transfer op
    // between its EAGAIN CQE and its dispatch is invisible to the ring;
    // its snapshot of this is what stops it re-arming.
    std::atomic<std::uint32_t> epoch_{0};

    uring_descriptor_read_op rd_;
    uring_descriptor_write_op wr_;
    uring_wait_op wait_rd_;
    uring_wait_op wait_wr_;
    uring_wait_op wait_er_;

public:
    uring_descriptor(
        uring_descriptor_service& svc, uring_scheduler& sched) noexcept
        : svc_(&svc)
        , sched_(&sched)
    {
    }

    ~uring_descriptor() override
    {
        close_descriptor();
    }

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after uring_descriptor_service for its complete type.
    void retire() noexcept override;

    /** Assert the closed state a recycled descriptor starts from.

        `close_descriptor()` already drove fd_ to its closed value
        before the refcount reached zero; each op's own `prepare()`
        overwrites its state before the next use.

        @pre refs_ == 0, fd closed, no op in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(fd_ == -1);
        BOOST_COROSIO_ASSERT(!rd_.stop_cb);
        BOOST_COROSIO_ASSERT(!wr_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_rd_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_wr_.stop_cb);
        BOOST_COROSIO_ASSERT(!wait_er_.stop_cb);
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
            detail::object_ref(this), buffers, token);
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
            detail::object_ref(this), buffers, token);
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

        uring_submit_op(*sched_, &wr_);
        return std::noop_coroutine();
    }

    // -- posix_stream_descriptor::implementation --

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
            h, ex, ec, fd_, sched_, detail::object_ref(this), poll_flags, token);
        sched_->work_started();

        if (fd_ < 0)
        {
            op->res = -EBADF;
            push_completed(op);
            return std::noop_coroutine();
        }

        // Without poll support there is no error condition to watch;
        // the kernel would refuse the poll with EINVAL.
        if (w == wait_type::error && !pollable_)
        {
            op->res = -EOPNOTSUPP;
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
        // Bump before the flush, as close_descriptor does. Flush the
        // cancel while the fd is still open so the kernel resolves it
        // before the caller can close and recycle the number. Do NOT
        // close -- the caller takes ownership.
        epoch_.fetch_add(1, std::memory_order_release);
        if (fd_ >= 0)
            sched_->cancel_and_flush(fd_);
        native_handle_type released = fd_;
        fd_                         = -1;
        return released;
    }

    void cancel() noexcept override
    {
        // Bump before the SQE: cancel-by-fd reaches only what the ring
        // currently holds, and an op waiting for its handler to run
        // holds nothing there. The epoch is what that op consults.
        epoch_.fetch_add(1, std::memory_order_release);
        if (fd_ >= 0)
            sched_->submit_cancel_by_fd(fd_);
    }

    /// Epoch bumped by every @ref cancel and every descriptor change.
    std::uint32_t epoch() const noexcept
    {
        return epoch_.load(std::memory_order_acquire);
    }

    // -- Service-facing (non-virtual) --

    /** Adopt an already-validated descriptor.

        @param fd The descriptor to adopt.
        @param pollable Whether the kernel can poll @p fd; see
            @ref fd_is_pollable.
    */
    void set_descriptor(int fd, bool pollable = true) noexcept
    {
        epoch_.fetch_add(1, std::memory_order_release);
        fd_       = fd;
        pollable_ = pollable;
    }

    /// Whether the kernel can poll the held descriptor.
    bool pollable() const noexcept
    {
        return pollable_;
    }

    /// Teardown hook named by uring_file_service_base.
    void close_file() noexcept
    {
        close_descriptor();
    }

    /// Cancel pending operations and close the descriptor. No-op when
    /// already closed.
    void close_descriptor() noexcept
    {
        if (fd_ < 0)
            return;
        // Bump before the flush: an op prepping concurrently must see
        // the change, or its SQE could land on a recycled fd number.
        epoch_.fetch_add(1, std::memory_order_release);
        // Both kernel entries below can run a queued pipe write as task
        // work; with the reader already gone that raises SIGPIPE.
        scoped_sigpipe_block no_sigpipe;
        sched_->cancel_and_flush(fd_);
        ::close(fd_);
        fd_ = -1;
    }

private:
    /** Bind a transfer slot to this descriptor for a fresh submission.

        The epoch snapshot taken here is what every later prep compares
        against; see uring_descriptor_continue.
    */
    template<class Op>
    void arm_slot(Op& op) noexcept
    {
        op.desc      = this;
        op.polling   = false;
        op.abandoned = false;
        op.epoch     = epoch_.load(std::memory_order_acquire);
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
uring_descriptor_abandoned(Op const& op) noexcept
{
    return op.cancelled.load(std::memory_order_acquire) ||
        op.desc->epoch() != op.epoch;
}

template<class Op>
bool
uring_descriptor_continue(Op& op) noexcept
{
    // A NOP completed: the op was abandoned at prep.
    if (op.abandoned)
    {
        op.res = -ECANCELED;
        return false;
    }

    // A failed poll, a transfer error or a byte count is the operation's
    // answer, and bytes beat a cancel that landed after them.
    bool const rearm = op.polling
        ? op.res >= 0
        : (op.res == -EAGAIN || op.res == -EWOULDBLOCK);
    if (!rearm)
    {
        // A poll the full SQ never took comes back as -EAGAIN, and a
        // transfer the cancel interrupted in an io-wq worker as -EINTR;
        // either one abandoned meanwhile is owed canceled, as above.
        bool const interrupted =
            op.polling ? op.res == -EAGAIN : op.res == -EINTR;
        if (interrupted && uring_descriptor_abandoned(op))
            op.res = -ECANCELED;
        return false;
    }

    // Abandoned since the CQE arrived: the caller is owed canceled,
    // never the poll's revents or the transfer's EAGAIN.
    if (uring_descriptor_abandoned(op))
    {
        op.res = -ECANCELED;
        return false;
    }

    // A file with no poll support is ready to every poll, so arming
    // one would retry the refused transfer on a CPU forever. The
    // reactors report the same refusal.
    if (!op.polling && !op.desc->pollable())
    {
        op.res = -EOPNOTSUPP;
        return false;
    }
    op.polling = !op.polling;

    // do_one spends a work_finished() on every op it dispatches, so an
    // op going round again has to be counted again. Nothing may touch
    // op after the submit: another thread can complete and free it.
    op.sched_->work_started();
    uring_submit_op(*op.sched_, &op);
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
