//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

// Include before any project headers open a namespace — prevents the
// boost::corosio::uring tag variable from shadowing struct ::io_uring.
#include <liburing.h>

#include <boost/corosio/detail/conditionally_enabled_event.hpp>
#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/ready_queue.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/uring/uring_op.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/abandon.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <initializer_list>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** Block SIGPIPE on the calling thread for the current scope.

    A queued pipe write can execute inline while this thread is in
    the kernel submitting SQEs; if the pipe's reader is already gone
    the kernel raises a thread-directed SIGPIPE, which kills any
    process that has not ignored the signal. The write's CQE still
    reports `EPIPE`, so the signal carries no information the
    completion path does not already deliver. The destructor consumes
    any SIGPIPE raised while blocked and restores the caller's mask.
*/
class scoped_sigpipe_block
{
    sigset_t old_{};
    bool restore_ = false;

public:
    /// Consume any SIGPIPE raised in scope and restore the mask.
    ~scoped_sigpipe_block()
    {
        if (!restore_)
            return;
        if (!sigismember(&old_, SIGPIPE))
        {
            // Only consume what this scope could have generated; a
            // caller who blocked SIGPIPE keeps their pending state.
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGPIPE);
            timespec zero{};
            while (::sigtimedwait(&set, nullptr, &zero) == SIGPIPE)
            {
            }
        }
        ::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }

    /// Construct and block SIGPIPE for the calling thread.
    scoped_sigpipe_block() noexcept
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        restore_ = ::pthread_sigmask(SIG_BLOCK, &set, &old_) == 0;
    }

    scoped_sigpipe_block(scoped_sigpipe_block const&)            = delete;
    scoped_sigpipe_block& operator=(scoped_sigpipe_block const&) = delete;
};

// Forward-declared so the out-of-line inline definitions below the class
// can reference the frame stack without a circular dependency.
struct uring_scheduler_frame;
extern thread_local uring_scheduler_frame* tl_running_scheduler_frame_;

/** io_uring scheduler — proactor model on Linux 6.x+.

    Owns one io_uring per io_context. Lazy batched submit;
    cross-thread post wakes a registered eventfd via multishot
    POLL_ADD.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL uring_scheduler final
    : public scheduler
{
public:
    using mutex_type = conditionally_enabled_mutex;
    using lock_type  = mutex_type::scoped_lock;
    using event_type = conditionally_enabled_event;

    uring_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);
    ~uring_scheduler() override;
    uring_scheduler(uring_scheduler const&)            = delete;
    uring_scheduler& operator=(uring_scheduler const&) = delete;

    void shutdown() override;

    void post(std::coroutine_handle<>) const override;
    void post(scheduler_op*) const override;
    void post(capy::continuation&) const override;

    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;
    void work_started() noexcept override;
    void work_finished() noexcept override;

    /// Watch the read end of the POSIX signal self-pipe (see scheduler.hpp).
    /// Submits a multishot POLL on @p read_fd; on readiness the drain+deliver
    /// runs in dispatch context via signal_drain_op_.
    [[nodiscard]] std::error_code register_signal_reader(int read_fd) override;

    /** Return the underlying liburing ring.

        Triggers lazy ring initialisation on first call. Used by
        socket op submission helpers (e.g. `uring_submit_op`) and
        any other code path that needs a live ring pointer.
    */
    struct ::io_uring* ring() noexcept
    {
        lazy_init_ring();
        return &ring_;
    }

    /// Return the dispatch mutex (protects completed_ops_ / cond_).
    mutex_type& dispatch_mutex() const noexcept
    {
        return dispatch_mutex_;
    }

    /// Return the ring mutex (serialises userspace SQ/CQ access).
    mutex_type& ring_mutex() const noexcept
    {
        return ring_mutex_;
    }

    /** Reset the calling thread's inline-budget for this scheduler.

        Called at the top of each dispatched op in `do_one` so each
        op handler gets a fresh budget for inline speculative
        completions. Walks the frame stack; no-op if this scheduler
        isn't on the stack (i.e. called from a non-run thread).
    */
    void reset_inline_budget() const noexcept;

    /** Consume one unit of inline budget if available.

        @return `true` if budget was available and consumed; `false`
            if the budget is exhausted or this scheduler is not on
            the calling thread's run stack.
    */
    bool try_consume_inline_budget() const noexcept;

    /// Exchange the submit-batch posted flag. Returns the prior value.
    /// Caller MUST hold ring_mutex_.
    bool submit_op_posted_exchange(bool desired) const noexcept
    {
        return submit_op_posted_.exchange(desired, std::memory_order_relaxed);
    }

    /** Check whether a stream write should join the queued batch.

        A write that would otherwise be issued directly can instead
        ride the batch submit's kernel entry. Only a single running
        thread joins: with more, the batch may run on another thread,
        which would then own this write's completion. The answer is a
        hint and may be stale.

        @return `true` if a batch submit is queued and exactly one
            thread is running the scheduler.
    */
    bool write_may_join_batch() const noexcept
    {
        return submit_op_posted_.load(std::memory_order_relaxed) &&
            runners_.load(std::memory_order_relaxed) == 1;
    }

    /// Return the number of threads running the scheduler.
    int runners() const noexcept
    {
        return runners_.load(std::memory_order_relaxed);
    }

    /// Record a thread entering a run function.
    void runner_entered() const noexcept
    {
        runners_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Record a thread leaving a run function.
    void runner_exited() const noexcept
    {
        runners_.fetch_sub(1, std::memory_order_relaxed);
    }

    /// Return a reference to the mutable embedded submit_sqes_op.
    scheduler_op& submit_op_ref() const noexcept
    {
        return submit_op_;
    }

    /// Increment the io_uring in-flight counter. Callers prep an SQE
    /// whose CQE must be reaped. Excluded: the wakeup-eventfd poll,
    /// which stays armed for the ring's lifetime.
    void inflight_inc() const noexcept
    {
        uring_inflight_.fetch_add(1, std::memory_order_release);
    }

    /** Return the current io_uring in-flight counter.

        Test-only helper: `uring_inflight_` is an internal accounting
        counter with no bearing on the public API. It is exposed solely
        so tests can assert the counter stays balanced across op
        submission and teardown. Not thread-safe with
        respect to a concurrently running scheduler; call it from a quiesced
        context.

        @return The number of SQEs currently counted as in flight.
    */
    std::int64_t inflight() const noexcept
    {
        return uring_inflight_.load(std::memory_order_acquire);
    }

    /// Initialize the io_uring ring on first access. Idempotent.
    void lazy_init_ring() const;

    /** Create the io_uring ring.

        Called once the owning `io_context` has applied every option
        that feeds the ring's setup flags, so that a kernel that
        refuses the ring is reported from the `io_context`
        constructor rather than from the first operation submitted
        through it. Idempotent.

        @par Exception Safety
        Strong guarantee. A ring that cannot be created leaves no
        descriptor behind.

        @throws std::system_error If the ring, its wakeup eventfd, or
            the poll watching that eventfd could not be set up.
    */
    void init_ring() const
    {
        lazy_init_ring();
    }

    /// Wake the leader if it's blocked in `submit_and_wait_timeout`.
    /// Best-effort: the wakeup is suppressed if the leader has already
    /// been signalled and not yet acked.
    void interrupt_reactor() const noexcept;

    /** Wake a thread to run newly queued work.

        Called under `dispatch_mutex_`: the caller may be on another
        context's thread, and once the lock is released the work can
        run, finish and let this scheduler be destroyed.
    */
    void wake_one_locked() const noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` targeting an in-flight op by its
        user_data pointer.

        The kernel delivers `-ECANCELED` on the target's CQE if it was
        still in flight; the op's completion handler then reports
        `operation_aborted`.  Best-effort: if the SQ is full after one
        flush attempt the function returns without cancelling (the op
        will complete normally on its own).

        @param target The in-flight op to cancel.
    */
    void submit_cancel_by_user_data(uring_op* target) noexcept;

    /** Cancel @p op by its user_data if the kernel may hold it.

        A cancel that cannot be queued yet is deferred, and queued
        ahead of the next operation's SQE.

        @pre `ring_mutex()` is held.

        @param op The operation to cancel.
    */
    void cancel_op_locked(uring_op* op) noexcept;

    /** Cancel each of @p ops the kernel may hold.

        Only these operations are cancelled, never another object's
        operation on the same file.

        @param ops The operations to cancel.
    */
    void cancel_ops(std::initializer_list<uring_op*> ops) noexcept;

    /** Cancel an object's operations.

        @param cancel Called under the ring mutex; cancels the object's
            operations with @ref cancel_op_locked.
    */
    template<class Cancel>
    void cancel_ops(Cancel&& cancel) noexcept
    {
        lazy_init_ring();
        {
            lock_type lock(ring_mutex_);
            cancel();
        }
        // After queuing: a leader woken first could re-enter its wait
        // before the cancel SQE exists, leaving it unsubmitted.
        interrupt_reactor();
    }

    /** Cancel an object's operations and flush every SQE naming `fd`.

        Once this returns no queued SQE refers to `fd` by number, so
        the caller may close it, or hand it to an owner who may. If the
        flush fails, this object's SQEs still queued are turned into
        no-ops and their operations complete as cancelled.

        @param fd The object's descriptor.
        @param cancel Called under the ring mutex; cancels the object's
            operations with @ref cancel_op_locked.
    */
    template<class Cancel>
    void flush_after_cancel(int fd, Cancel&& cancel) noexcept;

    /// @copydoc flush_after_cancel
    void
    flush_after_cancel(int fd, std::initializer_list<uring_op*> ops) noexcept
    {
        // An op clears sqe_set only once the kernel is done with its
        // entry, so with none set nothing names `fd`. Closing an idle
        // socket then skips the flush's kernel entries and the wake.
        bool armed = false;
        for (auto* op : ops)
            armed = armed || op->sqe_set.load(std::memory_order_acquire);
        if (!armed)
            return;

        flush_after_cancel(fd, [&]() noexcept {
            for (auto* op : ops)
                cancel_op_locked(op);
        });
    }

    /** Return a submission queue entry for a new operation.

        Deferred cancels are queued first: they name operations by
        address, and an operation armed later at the same address must
        not be what they reach. Flushes once if the queue is full.

        @pre `ring_mutex()` is held.

        @return The entry, or null if the queue stays full or a
            deferred cancel still cannot be queued.
    */
    ::io_uring_sqe* acquire_sqe() noexcept;

    /** Queue an already-counted op while the caller holds dispatch_mutex_.

        Does NOT increment `outstanding_work_`. Use for synchronous
        completion paths (e.g. SQE backpressure) where the caller called
        `work_started()` and already holds the dispatch lock.

        @pre `dispatch_mutex_` must be locked by the calling thread.
    */
    void push_completed_locked(scheduler_op* op) const noexcept
    {
        completed_ops_.push(op);
        // A run thread finds the op on its next pass; any other caller
        // must wake one, or it waits for the nearest timer.
        if (!running_in_this_thread())
            wake_one_locked();
    }

    void configure_threading(threading_config cfg) noexcept override
    {
        scheduler_locking_disabled_ = !cfg.scheduler_locking;
        // reactor_io_locking off also drives the eventfd wake elision (see
        // interrupt_reactor). one_thread is unused: the leader-follower wake
        // model is not gated on it.
        reactor_io_locking_ = cfg.reactor_io_locking;
        dispatch_mutex_.set_enabled(cfg.scheduler_locking);
        ring_mutex_.set_enabled(cfg.reactor_io_locking);
        cond_.set_enabled(cfg.scheduler_locking);
    }

    /** Configure SQPOLL parameters.

        Must be called before the ring is created — the values are
        cached and read when it is. No-op if `enable` is false (the
        default).

        @param enable    Set IORING_SETUP_SQPOLL on ring init.
        @param idle_ms   sq_thread_idle in milliseconds; 0 = kernel
                         default (1ms).
        @param cpu       Pin the polling thread to this CPU; -1 to
                         not pin.
    */
    void configure_sqpoll(bool enable, unsigned idle_ms, int cpu) noexcept
    {
        enable_sqpoll_     = enable;
        sq_thread_idle_ms_ = idle_ms;
        sq_thread_cpu_     = cpu;
    }

    /// Return true when scheduler locking is disabled (fully-lockless tier).
    bool scheduler_locking_disabled() const noexcept override
    {
        return scheduler_locking_disabled_;
    }

private:
    void on_timer_service_attached(timer_service& svc) override;

    /// Destroy every queued operation without running it.
    void destroy_queued(lock_type& lock) noexcept;

    // ring_ + wakeup_eventfd_ are mutable so lazy_init_ring() (called
    // from const contexts like post()) can populate them on first use.
    mutable struct ::io_uring ring_{};
    mutable int wakeup_eventfd_ = -1;

    // dispatch_mutex_ protects completed_ops_, cond_, task_running_.
    // ring_mutex_ protects every userspace touch of ring_ (SQ tail,
    // CQ head): get_sqe / submit / submit_and_wait_timeout /
    // for_each_cqe / cq_advance.
    //
    // process_completions runs under ring_mutex_ and briefly takes
    // dispatch_mutex_ to splice into completed_ops_. The locks are
    // never held simultaneously for the full duration of any other
    // path's critical section, so no deadlock.
    mutable mutex_type dispatch_mutex_{true};
    mutable mutex_type ring_mutex_{true};
    mutable event_type cond_{true};
    mutable ready_queue completed_ops_;
    // outstanding_work_ and uring_inflight_ are both atomic
    // counters updated at high frequency on different paths:
    //   - outstanding_work_ : every work_started / work_finished call,
    //                         including timers, posts, and SQE submits.
    //   - uring_inflight_ : only SQE submit + non-F_MORE CQE consume.
    // Under multi-thread workloads the threads tend to update these
    // from different code paths; placing them on the same cache line
    // would cause false sharing and unnecessary cache-line ping-pong.
    // Hold each on its own line.
    alignas(64) mutable std::atomic<std::int64_t> outstanding_work_{0};
    // Count of io_uring SQEs in flight whose completion must be reaped.
    // Excludes the wakeup-eventfd poll (registered in lazy_init_ring), and
    // is updated by uring_submit_op and by process_completions on
    // each non-F_MORE, non-eventfd CQE. shutdown() waits for it to drain.
    alignas(64) mutable std::atomic<std::int64_t> uring_inflight_{0};
    std::atomic<bool> stopped_{false};
    // True while a thread holds task_op_ and runs a ring pass.
    // Protected by dispatch_mutex_.
    mutable bool task_running_       = false;
    bool scheduler_locking_disabled_ = false;
    bool reactor_io_locking_         = true;
    bool enable_sqpoll_              = false;
    unsigned sq_thread_idle_ms_      = 0;
    int sq_thread_cpu_               = -1;

    int cancel_sentinel_ = 0;

    std::vector<uring_op*> deferred_cancels_; // guarded by ring_mutex_
    std::atomic<bool> has_deferred_cancels_{false};

    /// io_uring_submit, retried while a signal or a full completion
    /// queue delays it. @pre ring_mutex_ is held.
    int submit_retrying() noexcept;

    // Submit as submit_retrying() does, and under SQPOLL also wait for
    // the kernel thread to take every queued entry: an SQE names its
    // file by number until it is issued, so the number must stay open
    // until then.
    int submit_issued() noexcept;

    /// Queue a cancel of @p op by user_data; return false if the SQ
    /// stays full. @pre ring_mutex_ is held.
    bool queue_cancel_locked(uring_op* op) noexcept;

    /// Queue the deferred cancels; return false if one still cannot
    /// be. @pre ring_mutex_ is held.
    bool queue_deferred_cancels() noexcept;

    /// Turn every SQE on @p fd the kernel has not taken into a no-op,
    /// completing its operation as cancelled. @pre ring_mutex_ is held.
    void neutralize_pending_locked(int fd) noexcept;

    // Signal self-pipe integration. The read end is watched via a multishot
    // POLL SQE tagged with &signal_pipe_sentinel_ (distinct from nullptr =
    // wakeup eventfd and &cancel_sentinel_). On its CQE we re-arm the poll if
    // needed and enqueue signal_drain_op_ so the drain+deliver runs in
    // dispatch context — never under ring_mutex_ — keeping deliver_signal's
    // mutex locking off the ring critical section.
    int signal_pipe_read_fd_  = -1;
    int signal_pipe_sentinel_ = 0;

    /// Dispatch-context op that drains the signal self-pipe and delivers each
    /// pending signal. Enqueued (once at a time, guarded by queued_) from
    /// process_completions when the poll CQE fires. Scheduler-owned; destroy()
    /// is a no-op.
    struct signal_drain_op final : scheduler_op
    {
        std::atomic<bool> queued_{false};

        void operator()() override
        {
            // Clear before draining so a signal that arrives mid-drain re-arms
            // the op via a fresh CQE rather than being lost.
            queued_.store(false, std::memory_order_release);
            posix_signal_detail::drain_signal_pipe();
        }

        void destroy() override {}
    };
    mutable signal_drain_op signal_drain_op_;

    /// Flushes the SQ ring and drains CQEs in one mutex-held pass.
    /// One instance covers a whole batch; subsequent SQEs in the same
    /// batch skip the post, amortising syscall cost across the batch.
    /// Mirrors Asio's `submit_sqes_op` (`io_uring_service.ipp:730-742`).
    struct submit_sqes_op final : scheduler_op
    {
        uring_scheduler* sched_ = nullptr;

        submit_sqes_op() noexcept : scheduler_op(&do_handler) {}

        // do_one dispatches through operator(); do_handler alone
        // serves only the destroy path.
        void operator()() override
        {
            do_handler(sched_, this, 0, 0);
        }

        static void do_handler(
            void* owner,
            scheduler_op* base,
            std::uint32_t /*bytes*/,
            std::uint32_t /*error*/) noexcept;
    };

    /// True between the first submitter of a batch posting `submit_op_`
    /// and the dispatched op clearing the flag inside its handler.
    /// Written only while holding `ring_mutex_`.
    mutable std::atomic<bool> submit_op_posted_{false};

    mutable std::atomic<int> runners_{0};

    /// Single embedded `submit_sqes_op` instance, owned by the scheduler.
    mutable submit_sqes_op submit_op_;

    /// Sentinel op that triggers a ring pass when dequeued.
    struct task_op final : scheduler_op
    {
        // LCOV_EXCL_START: the sentinel is intercepted by pointer
        // identity; its virtuals exist for vtable completeness.
        void operator()() override {}
        void destroy() override {}
        // LCOV_EXCL_STOP
    };

    // Exactly one instance circulates through completed_ops_, so the
    // ring is entered once per pass over the queue rather than once
    // per handler.
    mutable task_op task_op_;

    // ring_inited_ goes true once the ring exists. The init is deferred
    // from the constructor so configure_threading() and configure_sqpoll()
    // can take effect before io_uring_queue_init_params chooses flags.
    mutable std::once_flag ring_init_once_;
    mutable bool ring_inited_ = false;

    std::size_t do_one(long timeout_us);

    /** Reap the ring once, then expire due timers.

        A zero timeout only peeks the completion queue, which needs no
        kernel entry unless the kernel holds completions back.

        @throws std::system_error If the kernel wait fails.
    */
    void run_task(long timeout_us);

    void process_completions();
    void drain_wakeup_eventfd() const noexcept;
    bool prep_multishot_poll(int fd, void* data) noexcept;
    void lazy_init_ring_unlocked() const;
};

inline uring_scheduler::uring_scheduler(
    capy::execution_context&, int /*concurrency_hint*/)
{
    // sched_ cannot be set in the member initialiser — `this` is not
    // available there.
    submit_op_.sched_ = this;
    completed_ops_.push(&task_op_);

    // Ring init is deferred so the options the io_context applies
    // after this constructor — the locking tier and SQPOLL — can feed
    // the flags io_uring_queue_init_params is given. The io_context
    // calls init_ring() once they are in place; the lazy_init_ring()
    // calls scattered through the op paths only matter for a
    // scheduler used without one.
}

inline uring_scheduler::~uring_scheduler()
{
    // Services give up their work in shutdown(), but anything posted
    // after the drain, such as from a destructor, is still destroyed.
    {
        lock_type lock(dispatch_mutex_);
        destroy_queued(lock);
    }
    if (ring_inited_)
    {
        // Ring teardown can still run a doomed pipe write as task
        // work; absorb the SIGPIPE it would raise.
        scoped_sigpipe_block no_sigpipe;
        if (wakeup_eventfd_ >= 0)
            ::close(wakeup_eventfd_);
        ::io_uring_queue_exit(&ring_);
    }
}

inline void
uring_scheduler::lazy_init_ring() const
{
    std::call_once(ring_init_once_, [this] { lazy_init_ring_unlocked(); });
}

inline void
uring_scheduler::lazy_init_ring_unlocked() const
{
    // No flags bind the ring to one thread, even in the lockless tiers,
    // so a context may be made, run and destroyed on different threads,
    // and its teardown can always drain the ring.
    io_uring_params params{};

    if (enable_sqpoll_)
    {
        // SQPOLL forks a kernel thread that busy-polls the SQ ring;
        // submission becomes a userspace-only memory store. Idle
        // timeout 0 means kernel default (1ms); we only forward when
        // explicitly set so the kernel default is preserved.
        params.flags |= IORING_SETUP_SQPOLL;
        if (sq_thread_idle_ms_ != 0)
            params.sq_thread_idle = sq_thread_idle_ms_;
        if (sq_thread_cpu_ >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = static_cast<__u32>(sq_thread_cpu_);
        }
    }

    // SUBMIT_ALL keeps a batch going past an entry the kernel rejects,
    // so a cancel queued behind it is still issued.
    //
    // The kernel finishes a socket completion on the thread that
    // submitted its entry, and a batch submit makes one thread the
    // submitter for its peers' entries too. Without COOP_TASKRUN the
    // kernel interrupts that thread for each completion while it runs
    // handlers; with it, the work waits for the thread's next kernel
    // entry, and a sleeping thread is still woken. TASKRUN_FLAG marks
    // that waiting work in the ring, so a pass that only peeks still
    // enters the kernel to finish it rather than missing completions
    // while handlers keep the thread in user space. SQPOLL rings already
    // work that way and refuse both flags.
    //
    // Kernels that predate a flag refuse it. The two TASKRUN flags are
    // the newer, so they are dropped first.
    io_uring_params const base = params;
    unsigned const coop        = enable_sqpoll_
        ? 0u
        : IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
    params.flags |= IORING_SETUP_SUBMIT_ALL | coop;
    int rc = ::io_uring_queue_init_params(256, &ring_, &params);
    if (rc == -EINVAL && coop != 0)
    {
        params = base;
        params.flags |= IORING_SETUP_SUBMIT_ALL;
        rc = ::io_uring_queue_init_params(256, &ring_, &params);
    }
    if (rc == -EINVAL)
    {
        params = base;
        rc     = ::io_uring_queue_init_params(256, &ring_, &params);
    }
    if (rc < 0)
        detail::throw_system_error(make_err(-rc), "io_uring_queue_init_params");

    wakeup_eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_eventfd_ < 0)
    {
        int errn = errno;
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(make_err(errn), "eventfd");
    }

    // Register a one-shot poll on the wake eventfd. user_data nullptr
    // is the sentinel recognized by process_completions, which calls
    // drain_wakeup_eventfd() to consume the eventfd byte AND re-arm
    // the poll. Plan 5a switched away from IORING_POLL_MULTISHOT
    // because multishot ops can silently terminate (e.g. under CQ
    // pressure), and we don't observe the termination — leaving the
    // wake mechanism dead and the leader stuck in kernel wait. One-
    // shot rearm-on-fire is fail-fast: every wake event is paired
    // with an explicit rearm, so a missed rearm would manifest
    // immediately as the next wake being lost (test-visible).
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        ::close(wakeup_eventfd_);
        wakeup_eventfd_ = -1;
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(
            make_err(ENOSPC), "io_uring_get_sqe (wakeup)");
    }
    // Multishot poll: fires a CQE on each eventfd POLLIN without
    // consuming the SQE. Avoids the re-arm hazard of one-shot poll
    // (where drain_wakeup_eventfd's get_sqe could return null on a
    // full SQ, leaving no SQE to detect future wakes).
    ::io_uring_prep_poll_multishot(sqe, wakeup_eventfd_, POLLIN);
    ::io_uring_sqe_set_data(sqe, nullptr);
    int submit_rc = ::io_uring_submit(&ring_);
    if (submit_rc < 0)
    {
        ::close(wakeup_eventfd_);
        wakeup_eventfd_ = -1;
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(
            make_err(-submit_rc), "io_uring_submit (wakeup)");
    }

    ring_inited_ = true;
}

inline void
uring_scheduler::shutdown()
{
    stopped_.store(true, std::memory_order_release);

    // Cancel every request still in the kernel and wait for each to
    // come back: the services free their impls next, and an op still
    // armed would write into freed memory. One cancel-any covers ops
    // whose own service-side cancel could not be queued. The kernel
    // answers every cancel, so once it is submitted the wait ends; a
    // ring that will not take it leaves nothing that could end a wait.
    // A signal or a full completion queue only delays either step.
    if (ring_inited_)
    {
        lock_type ring_lock(ring_mutex_);
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
        if (!sqe)
        {
            submit_issued();
            sqe = ::io_uring_get_sqe(&ring_);
        }
        if (sqe)
        {
            ::io_uring_prep_cancel64(sqe, 0, IORING_ASYNC_CANCEL_ANY);
            ::io_uring_sqe_set_data(sqe, &cancel_sentinel_);
            inflight_inc();
        }
        int const submitted = sqe ? submit_retrying() : -EINVAL;
        while (submitted >= 0 &&
               uring_inflight_.load(std::memory_order_acquire) > 0)
        {
            ::io_uring_cqe* cqe = nullptr;
            int const rc = ::io_uring_wait_cqe_timeout(&ring_, &cqe, nullptr);
            if (rc == -EINTR || rc == -EAGAIN || rc == -ETIME)
                continue;
            if (rc != 0)
                break;
            process_completions();
        }

        deferred_cancels_.clear();
        has_deferred_cancels_.store(false, std::memory_order_release);
    }

    wait_cancels_posting();

    // Drain posted ops, including the completions reaped above,
    // calling destroy() on each so embedded handles (coroutine frames,
    // error_code outputs) get torn down rather than leaked. Mirrors
    // reactor_scheduler::shutdown_drain.
    lock_type lock(dispatch_mutex_);
    destroy_queued(lock);
    cond_.notify_all();
}

inline void
uring_scheduler::destroy_queued(lock_type& lock) noexcept
{
    while (auto e = completed_ops_.pop())
    {
        if (ready_is_continuation(e))
        {
            lock.unlock();
            if (auto* c = ready_as_cont(e); c->h)
                capy::abandon(*c);
            lock.lock();
        }
        else
        {
            auto* op = ready_as_op(e);
            if (op == &task_op_)
                continue;
            lock.unlock();
            op->destroy();
            lock.lock();
        }
    }
}

inline void
uring_scheduler::stop()
{
    // All under the lock: releasing the last work on another thread
    // gets here, and a run thread that sees stopped_ may return and
    // let this scheduler be destroyed.
    lock_type lock(dispatch_mutex_);
    stopped_.store(true, std::memory_order_release);
    cond_.notify_all();
    // Force-wake unconditionally — bypass interrupt_reactor's CAS
    // coalescing. A dropped wake here leaves the leader blocked
    // forever in submit_and_wait_timeout (no further CQE will
    // arrive after stop()). With multishot poll on wakeup_eventfd_,
    // this write reliably produces a CQE.
    if (ring_inited_)
    {
        std::uint64_t v         = 1;
        [[maybe_unused]] auto r = ::write(wakeup_eventfd_, &v, sizeof(v));
    }
}

inline bool
uring_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

inline void
uring_scheduler::restart()
{
    stopped_.store(false, std::memory_order_release);
}

inline void
uring_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
uring_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

inline void
uring_scheduler::interrupt_reactor() const noexcept
{
    // Skip if the ring hasn't been initialised yet — there's no leader
    // to wake and no eventfd to write.
    if (!ring_inited_)
        return;

    // Only the unsafe tier forbids cross-thread post(); there the caller
    // is the run thread itself, which is not in a kernel wait. Under
    // unsafe_io a pool thread (the resolver) may post, and the eventfd
    // write leaves the single ring submitter untouched.
    if (scheduler_locking_disabled_)
        return;

    // Under unsafe_io a single thread runs the ring; when that thread is
    // the caller it is not in a kernel wait.
    if (!reactor_io_locking_ && running_scheduler.get() == this)
        return;

    // Multi-thread: write the eventfd unconditionally. CAS-coalescing
    // is unsafe here because the leader's Phase 2 in do_one waits
    // indefinitely for a CQE; a dropped wake leaves the leader
    // blocked forever when there is no other CQE-producing activity.
    // Multishot poll on wakeup_eventfd_ delivers a CQE for every
    // write, so multiple writes in flight produce multiple CQEs
    // (drained together by drain_wakeup_eventfd's single read of
    // the eventfd counter).
    std::uint64_t v = 1;
    std::ignore     = ::write(wakeup_eventfd_, &v, sizeof(v));
}

inline void
uring_scheduler::on_timer_service_attached(timer_service& svc)
{
    svc.set_on_earliest_changed(timer_service::callback(this, [](void* p) {
        static_cast<uring_scheduler*>(p)->interrupt_reactor();
    }));
}

inline void
uring_scheduler::drain_wakeup_eventfd() const noexcept
{
    std::uint64_t v;
    std::ignore = ::read(wakeup_eventfd_, &v, sizeof(v));

    // Multishot poll never needs re-arming. The poll-add was queued
    // once at lazy_init_ring with IORING_POLL_ADD_MULTI; each eventfd
    // POLLIN produces a CQE without consuming the SQE.
}

inline bool
uring_scheduler::prep_multishot_poll(int fd, void* data) noexcept
{
    // Prepare a multishot POLLIN SQE on `fd` tagged with `data`. Caller holds
    // ring_mutex_ and flushes separately (re-arm sites ride the batch submit;
    // register/init submit explicitly). Returns false when no SQE could be
    // had after one flush, so a caller that has someone to report to does
    // not mistake the submit of an empty queue for an armed poll. Shared by
    // the wakeup-eventfd and signal self-pipe multishot polls.
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        ::io_uring_submit(&ring_);
        sqe = ::io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return false;
    ::io_uring_prep_poll_multishot(sqe, fd, POLLIN);
    ::io_uring_sqe_set_data(sqe, data);
    return true;
}

inline std::error_code
uring_scheduler::register_signal_reader(int read_fd)
{
    // Called once per service from add_signal(), holding neither the
    // signal_state mutex nor the service mutex (see the call site). Submit a
    // multishot POLL on the pipe read end; its CQE (tagged with
    // &signal_pipe_sentinel_) is recognised in process_completions. The actual
    // drain+deliver is deferred to dispatch context, so the only lock taken
    // here is ring_mutex_ with no outer lock held, hence no lock-order
    // inversion with the reactor drain path.
    signal_pipe_read_fd_ = read_fd;
    lazy_init_ring();

    lock_type lock(ring_mutex_);
    // EAGAIN, not ENOBUFS: an exhausted submission queue reports the
    // same retryable code wherever it is hit, and the next add() is
    // the retry.
    if (!prep_multishot_poll(read_fd, &signal_pipe_sentinel_))
        return make_err(EAGAIN);
    int rc = ::io_uring_submit(&ring_);
    if (rc < 0)
        return make_err(-rc);
    return {};
}

inline void
uring_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;
        explicit post_handler(std::coroutine_handle<> h) noexcept : h_(h) {}

        void operator()() override
        {
            auto saved = h_;
            delete this;
            saved.resume();
        }

        void destroy() override
        {
            // A bare handle carries no owner to unwind from: it is
            // destroyed directly.
            auto saved = h_;
            delete this;
            if (saved)
                saved.destroy();
        }
    };

    auto* op = new post_handler(h);
    lazy_init_ring();
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.push(op);
        wake_one_locked();
    }
}

inline void
uring_scheduler::post(scheduler_op* op) const
{
    lazy_init_ring();
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.push(op);
        wake_one_locked();
    }
}

inline void
uring_scheduler::post(capy::continuation& c) const
{
    lazy_init_ring();
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.push(c);
        wake_one_locked();
    }
}

inline void
uring_scheduler::wake_one_locked() const noexcept
{
    if (!task_running_)
        cond_.notify_one();
    else
        interrupt_reactor();
}

// Thread-local stack of frames for io_uring schedulers being run on the
// current thread. Holds the running-scheduler pointer (for
// running_in_this_thread reporting) and the inline completion budget
// used by the speculative non-blocking I/O path (plan 5j). Nesting
// stacks frames via prev_ so each scheduler gets its own budget.
struct uring_scheduler_frame
{
    uring_scheduler const* sched;
    uring_scheduler_frame* prev;
    int inline_budget;
    int inline_budget_max;
};

inline thread_local uring_scheduler_frame* tl_running_scheduler_frame_ =
    nullptr;

// Default inline budget. Matches reactor's initial budget (2). Adaptive
// ramp-up to a max is intentionally NOT implemented yet — keep it simple
// for plan 5j and revisit if benches show fairness issues.
inline constexpr int uring_inline_budget_initial = 2;
inline constexpr int uring_inline_budget_max     = 16;

/// RAII guard: pushes a frame onto the thread's running-scheduler stack
/// on construction, restores the previous on destruction. Used by
/// run/run_one/wait_one/poll/poll_one to mark the running thread and
/// hold a fresh inline budget for speculative completions.
struct uring_run_guard
{
    uring_scheduler_frame frame_;
    running_scheduler_guard running_;

    explicit uring_run_guard(uring_scheduler const* self) noexcept
        : frame_{
              self, tl_running_scheduler_frame_, uring_inline_budget_initial,
              uring_inline_budget_max}
        , running_(self)
    {
        tl_running_scheduler_frame_ = &frame_;
        self->runner_entered();
    }

    ~uring_run_guard() noexcept
    {
        frame_.sched->runner_exited();
        tl_running_scheduler_frame_ = frame_.prev;
    }
};

inline bool
uring_scheduler::running_in_this_thread() const noexcept
{
    for (auto* f = tl_running_scheduler_frame_; f != nullptr; f = f->prev)
    {
        if (f->sched == this)
            return true;
    }
    return false;
}

inline void
uring_scheduler::reset_inline_budget() const noexcept
{
    for (auto* f = tl_running_scheduler_frame_; f != nullptr; f = f->prev)
    {
        if (f->sched == this)
        {
            f->inline_budget = f->inline_budget_max;
            return;
        }
    }
}

inline bool
uring_scheduler::try_consume_inline_budget() const noexcept
{
    for (auto* f = tl_running_scheduler_frame_; f != nullptr; f = f->prev)
    {
        if (f->sched == this)
        {
            if (f->inline_budget > 0)
            {
                --f->inline_budget;
                return true;
            }
            return false;
        }
    }
    return false;
}

inline std::size_t
uring_scheduler::run()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    uring_run_guard guard(this);
    std::size_t n = 0;
    for (;;)
    {
        std::size_t r = do_one(-1);
        if (r)
        {
            if (n != (std::numeric_limits<std::size_t>::max)())
                ++n;
            continue;
        }
        if (outstanding_work_.load(std::memory_order_acquire) == 0 ||
            stopped_.load(std::memory_order_acquire))
            break;
        // do_one returned 0 but work still outstanding (e.g. timer
        // expiry dispatched async work). Continue.
    }
    return n;
}

inline std::size_t
uring_scheduler::run_one()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    uring_run_guard guard(this);
    return do_one(-1);
}

inline std::size_t
uring_scheduler::wait_one(long usec)
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    uring_run_guard guard(this);
    return do_one(usec);
}

inline std::size_t
uring_scheduler::poll()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    uring_run_guard guard(this);
    std::size_t n = 0;
    while (do_one(0))
    {
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    }
    return n;
}

inline std::size_t
uring_scheduler::poll_one()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    uring_run_guard guard(this);
    return do_one(0);
}

inline std::size_t
uring_scheduler::do_one(long timeout_us)
{
    if (stopped_.load(std::memory_order_acquire))
    {
        // stop() sets the flag under the lock and still writes the
        // eventfd; returning before it unlocks lets the caller destroy
        // this scheduler under it.
        lock_type lock(dispatch_mutex_);
        return 0;
    }

    lock_type lock(dispatch_mutex_);
    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        auto const e = completed_ops_.pop();

        if (e && !ready_is_continuation(e) && ready_as_op(e) == &task_op_)
        {
            bool const more_handlers = !completed_ops_.empty();

            if (!more_handlers &&
                outstanding_work_.load(std::memory_order_acquire) == 0)
            {
                completed_ops_.push(&task_op_);
                return 0;
            }

            // A peer takes the queued handlers while this thread is in
            // the ring.
            if (more_handlers)
                cond_.notify_one();
            task_running_ = true;
            lock.unlock();

            try
            {
                // Queued handlers must not wait behind a kernel block.
                run_task(more_handlers ? 0 : timeout_us);
            }
            catch (...)
            {
                // Restore state so followers don't wait for a leader
                // that never returns.
                lock.lock();
                task_running_ = false;
                completed_ops_.push(&task_op_);
                cond_.notify_all();
                throw;
            }

            lock.lock();
            task_running_     = false;
            bool const reaped = !completed_ops_.empty();
            completed_ops_.push(&task_op_);

            // poll() and wait_one() make one ring pass, handing the
            // sentinel to a waiting peer.
            if (timeout_us >= 0 && !reaped)
            {
                cond_.notify_one();
                return 0;
            }
            continue;
        }

        if (e)
        {
            // Hand off any remaining queued work to a follower so we
            // dispatch in parallel.
            if (!completed_ops_.empty())
                cond_.notify_one();
            lock.unlock();
            // Speculative follow-ups in the handler share this budget.
            reset_inline_budget();
            if (ready_is_continuation(e))
                ready_as_cont(e)->h.resume();
            else
                (*ready_as_op(e))();
            work_finished();
            return 1;
        }

        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        // LCOV_EXCL_START: follower path — entered only when another
        // thread holds the sentinel at the instant this thread finds
        // the queue empty. That overlap is a race no deterministic
        // test reliably forces (see the io_uring mt_scheduler
        // regression test, which exercises MT run but does not pin
        // this window).
        if (timeout_us == 0)
            return 0;
        if (timeout_us < 0)
            cond_.wait(lock);
        else
        {
            cond_.wait_for(lock, std::chrono::microseconds(timeout_us));
            // wait_one honoured its timeout; if nothing arrived,
            // return rather than re-arm.
            if (completed_ops_.empty() &&
                !stopped_.load(std::memory_order_acquire))
                return 0;
        }
        // LCOV_EXCL_STOP
    }
}

inline void
uring_scheduler::run_task(long timeout_us)
{
    auto* tsvc = timers();

    if (timeout_us == 0)
    {
        lock_type ring_lock(ring_mutex_);
        if (has_deferred_cancels_.load(std::memory_order_acquire))
            queue_deferred_cancels();
        // A queued submit_op_ flushes its batch; anything else in the
        // SQ (cancels, poll re-arms) would otherwise wait for the
        // queue to drain.
        if (!submit_op_posted_.load(std::memory_order_relaxed) &&
            ::io_uring_sq_ready(&ring_) != 0)
            ::io_uring_submit_and_get_events(&ring_);
        else
        {
            ::io_uring_cqe* cqe = nullptr;
            std::ignore         = ::io_uring_peek_batch_cqe(&ring_, &cqe, 1);
        }
        process_completions();
    }
    else
    {
        __kernel_timespec ts{};
        __kernel_timespec* ts_ptr = nullptr;
        auto next_expiry =
            tsvc ? tsvc->nearest_expiry() : timer_service::time_point::max();
        auto now = std::chrono::steady_clock::now();

        if (next_expiry != timer_service::time_point::max())
        {
            // Timers expire after the wait, so a past deadline can be as
            // early as time_point::min(), where the subtraction overflows.
            std::chrono::nanoseconds::rep delta_ns = 0;
            if (next_expiry > now)
                delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               next_expiry - now)
                               .count();
            if (timeout_us > 0 && timeout_us < delta_ns / 1000)
                delta_ns = static_cast<decltype(delta_ns)>(timeout_us) * 1000;
            ts.tv_sec  = delta_ns / 1'000'000'000;
            ts.tv_nsec = delta_ns % 1'000'000'000;
            ts_ptr     = &ts;
        }
        else if (timeout_us > 0)
        {
            ts.tv_sec  = timeout_us / 1'000'000;
            ts.tv_nsec = (timeout_us % 1'000'000) * 1000;
            ts_ptr     = &ts;
        }
        else
        {
            // run() with no pending timers: cap the kernel wait at 1s
            // so the leader periodically re-checks state. Defense in
            // depth against a lost wakeup (e.g. multishot poll on the
            // wakeup eventfd terminates and the re-arm SQE doesn't
            // reach the kernel in time). Worst case: one extra
            // wake-up per io_context per second when truly idle.
            ts.tv_sec  = 1;
            ts.tv_nsec = 0;
            ts_ptr     = &ts;
        }

        // Three-phase kernel wait, matching Boost.Asio's
        // io_uring_service::run pattern. ring_mutex_ is held briefly
        // to push pending SQEs and to drain CQEs, but NOT during
        // the blocking io_uring_wait_cqe_timeout. Cross-thread
        // submitters (uring_submit_op, cancel paths) can take
        // ring_mutex_ during the wait and prep new SQEs without
        // blocking on the leader; their wake eventfd write fires the
        // multishot poll and returns the leader from wait_cqe_timeout
        // promptly.
        //
        // Phase 1 — submit any pending SQEs to the kernel.
        {
            lock_type ring_lock(ring_mutex_);
            if (has_deferred_cancels_.load(std::memory_order_acquire))
                queue_deferred_cancels();
            ::io_uring_submit(&ring_);
        }

        // Phase 2 — wait for at least one CQE without holding the
        // mutex. wait_cqe_timeout only peeks the CQ ring;
        // head advancement happens under the mutex in
        // process_completions below.
        ::io_uring_cqe* cqe = nullptr;
        int rc              = ::io_uring_wait_cqe_timeout(&ring_, &cqe, ts_ptr);

        // Phase 3 — drain CQEs under the mutex.
        // EBUSY: the CQ is full, so the kernel cannot flush overflowed
        // completions into it. Reaping makes room, so drain and wait
        // again.
        bool const transient = rc == -ETIME || rc == -EINTR || rc == -EBUSY;
        {
            lock_type ring_lock(ring_mutex_);
            if (rc == 0 || transient)
                process_completions();
        }

        if (rc < 0 && !transient)
            detail::throw_system_error(
                make_err(-rc), "io_uring_wait_cqe_timeout");
    }

    if (tsvc && !tsvc->empty())
        tsvc->process_expired();
}

inline void
uring_scheduler::process_completions()
{
    unsigned head;
    ::io_uring_cqe* cqe;
    unsigned consumed = 0;

    // Collect completed I/O ops locally; splice into completed_ops_
    // after the loop so do_one dispatches them one at a time.
    ready_queue local_ops;

    std::int64_t inflight_dec = 0;
    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        void* ud = io_uring_cqe_get_data(cqe);
        if (ud == nullptr)
        {
            // Wakeup eventfd CQE: drain the eventfd byte. Not counted
            // by uring_inflight_; we never incremented for the
            // wakeup multishot SQE (its progress doesn't depend on
            // userspace getevents).
            drain_wakeup_eventfd();
            // If multishot terminated (kernel dropped under memory
            // pressure or similar), re-arm. Each CQE except the last
            // sets IORING_CQE_F_MORE.
            // Best-effort: nothing here has a caller to report to, and
            // prep_multishot_poll already flushed once to make room.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
                std::ignore = prep_multishot_poll(wakeup_eventfd_, nullptr);
        }
        else if (ud == &cancel_sentinel_)
        {
            // CQE for an ASYNC_CANCEL op — ignore; the actual op's
            // CQE arrives separately and is dispatched via cqe_func.
            // Cancels are one-shot, no F_MORE, decrement inflight.
            ++inflight_dec;
        }
        else if (ud == &signal_pipe_sentinel_)
        {
            // Signal self-pipe readiness. Re-arm the multishot poll if it
            // terminated (F_MORE cleared), then enqueue signal_drain_op_ to
            // drain + deliver in dispatch context. Not counted in
            // uring_inflight_ (like the wakeup eventfd poll): it stays
            // armed for the ring's lifetime.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
                std::ignore = prep_multishot_poll(
                    signal_pipe_read_fd_, &signal_pipe_sentinel_);
            bool expected = false;
            if (signal_drain_op_.queued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                // Balance the work_finished() do_one runs after dispatching.
                work_started();
                local_ops.push(&signal_drain_op_);
            }
        }
        else
        {
            auto* iop = static_cast<uring_op*>(ud);
            // Nothing is left for a cancel to reach.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
                iop->sqe_set.store(false, std::memory_order_release);
            (*iop->cqe_func)(iop, cqe->res, cqe->flags, local_ops);
            // Decrement inflight on the terminal CQE only — multishot
            // ops (acceptor) hold the SQE alive across F_MORE CQEs and
            // free it only when F_MORE is cleared.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
                ++inflight_dec;
        }
        ++consumed;
    }
    if (inflight_dec)
        uring_inflight_.fetch_sub(inflight_dec, std::memory_order_acq_rel);

    if (consumed)
        io_uring_cq_advance(&ring_, consumed);

    // Caller holds ring_mutex_. Take dispatch_mutex_ briefly to
    // splice locally-collected ops onto the global queue (lock order
    // ring_mutex_ -> dispatch_mutex_).
    if (!local_ops.empty())
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.splice(local_ops);
        // Wake any follower waiting on cond_; it'll pop and dispatch.
        cond_.notify_one();
    }
}

inline void
uring_scheduler::submit_sqes_op::do_handler(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/) noexcept
{
    if (owner == nullptr)
        return; // shutdown drain — nothing to do; SQE storage is
                // kernel-mapped and discarded by io_uring_queue_exit.

    auto* self  = static_cast<submit_sqes_op*>(base);
    auto* sched = self->sched_;

    uring_scheduler::lock_type ring_lock(sched->ring_mutex_);
    sched->submit_op_posted_.store(false, std::memory_order_relaxed);
    ::io_uring_submit_and_get_events(&sched->ring_);
    sched->process_completions();
}

inline void
uring_scheduler::submit_cancel_by_user_data(uring_op* target) noexcept
{
    lazy_init_ring();
    {
        lock_type lock(ring_mutex_);
        cancel_op_locked(target);
    }
    // After queuing, so the woken leader's submit carries the cancel.
    interrupt_reactor();
}

inline bool
uring_scheduler::queue_cancel_locked(uring_op* target) noexcept
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        // A flush alone frees no space under SQPOLL until the kernel
        // thread consumes it.
        submit_issued();
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return false;

    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, &cancel_sentinel_);
    inflight_inc();
    return true;
}

inline void
uring_scheduler::cancel_op_locked(uring_op* op) noexcept
{
    if (!op->sqe_set.load(std::memory_order_acquire))
        return;
    if (queue_cancel_locked(op))
        return;
    deferred_cancels_.push_back(op);
    has_deferred_cancels_.store(true, std::memory_order_release);
}

inline void
uring_scheduler::cancel_ops(std::initializer_list<uring_op*> ops) noexcept
{
    cancel_ops([&]() noexcept {
        for (auto* op : ops)
            cancel_op_locked(op);
    });
}

template<class Cancel>
void
uring_scheduler::flush_after_cancel(int fd, Cancel&& cancel) noexcept
{
    // The flush can execute a queued write on `fd` inline; when the
    // fd is a pipe whose reader has already closed — service
    // shutdown closes impls one at a time, so teardown itself
    // creates that state — the kernel raises SIGPIPE.
    scoped_sigpipe_block no_sigpipe;

    lazy_init_ring();
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    cancel();
    if (submit_issued() < 0)
        neutralize_pending_locked(fd);
}

inline void
uring_op::on_cancel() noexcept
{
    // Paired with uring_link_sqe: with no SQE linked yet, the
    // initiator either sees the flag and completes without the kernel,
    // or links and queues the cancel itself.
    cancelled.store(true, std::memory_order_seq_cst);
    if (sched_ && sqe_set.load(std::memory_order_seq_cst))
        sched_->submit_cancel_by_user_data(this);
}

inline int
uring_scheduler::submit_retrying() noexcept
{
    int r;
    do
    {
        r = ::io_uring_submit(&ring_);
        if (r == -EBUSY)
            process_completions();
    }
    while (r == -EINTR || r == -EAGAIN || r == -EBUSY);
    return r;
}

inline int
uring_scheduler::submit_issued() noexcept
{
    bool const sqpoll = (ring_.flags & IORING_SETUP_SQPOLL) != 0;
    int r             = submit_retrying();
    // The kernel can take fewer entries than were flushed, and under
    // SQPOLL takes them later on its own thread.
    while (r >= 0 && ::io_uring_sq_ready(&ring_) > 0)
    {
        if (sqpoll)
            std::this_thread::yield();
        else if (r == 0)
            return -EBUSY;
        // Under SQPOLL this also wakes the kernel thread if it went idle.
        r = submit_retrying();
    }
    return r;
}

inline bool
uring_scheduler::queue_deferred_cancels() noexcept
{
    std::size_t kept = 0;
    for (auto* op : deferred_cancels_)
        if (!queue_cancel_locked(op))
            deferred_cancels_[kept++] = op;
    deferred_cancels_.resize(kept);
    has_deferred_cancels_.store(kept != 0, std::memory_order_release);
    return kept == 0;
}

inline ::io_uring_sqe*
uring_scheduler::acquire_sqe() noexcept
{
    if (has_deferred_cancels_.load(std::memory_order_relaxed) &&
        !queue_deferred_cancels())
        return nullptr;
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        ::io_uring_submit(&ring_);
        sqe = ::io_uring_get_sqe(&ring_);
    }
    return sqe;
}

inline void
uring_scheduler::neutralize_pending_locked(int fd) noexcept
{
    // Without SQPOLL the kernel reads the SQ only inside
    // io_uring_enter, which runs under ring_mutex_, so everything from
    // its head on is still ours. The SQPOLL thread reads published
    // entries on its own; only the unpublished ones are safe to touch.
    auto& sq          = ring_.sq;
    bool const sqpoll = (ring_.flags & IORING_SETUP_SQPOLL) != 0;
    unsigned const from =
        sqpoll ? sq.sqe_head : __atomic_load_n(sq.khead, __ATOMIC_ACQUIRE);

    ready_queue local_ops;
    for (unsigned i = from; i != sq.sqe_tail; ++i)
    {
        auto* sqe = &sq.sqes[i & sq.ring_mask];
        // NOLINTNEXTLINE(performance-no-int-to-ptr) — user_data holds the address io_uring_sqe_set_data stored
        auto* ud = reinterpret_cast<void*>(sqe->user_data);
        if (sqe->fd != fd || ud == nullptr || ud == &cancel_sentinel_ ||
            ud == &signal_pipe_sentinel_)
            continue;
        auto* op = static_cast<uring_op*>(ud);
        ::io_uring_prep_nop(sqe);
        ::io_uring_sqe_set_data(sqe, &cancel_sentinel_);
        op->sqe_set.store(false, std::memory_order_release);
        (*op->cqe_func)(op, -ECANCELED, 0, local_ops);
    }
    if (!local_ops.empty())
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.splice(local_ops);
        cond_.notify_one();
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_URING_URING_SCHEDULER_HPP
