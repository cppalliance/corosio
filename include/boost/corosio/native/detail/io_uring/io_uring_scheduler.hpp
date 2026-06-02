//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

// Include before any project headers open a namespace — prevents the
// boost::corosio::io_uring tag variable from shadowing struct ::io_uring.
#include <liburing.h>

#include <boost/corosio/detail/conditionally_enabled_event.hpp>
#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace boost::corosio::detail {

// Forward-declared so the out-of-line inline definitions below the class
// can reference the frame stack without a circular dependency.
struct io_uring_scheduler_frame;
extern thread_local io_uring_scheduler_frame* tl_running_scheduler_frame_;

/** io_uring scheduler — proactor model on Linux 6.x+.

    Owns one io_uring per io_context. Lazy batched submit;
    cross-thread post wakes a registered eventfd via multishot
    POLL_ADD.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL io_uring_scheduler final
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type   = scheduler;
    using mutex_type = conditionally_enabled_mutex;
    using lock_type  = mutex_type::scoped_lock;
    using event_type = conditionally_enabled_event;

    io_uring_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);
    ~io_uring_scheduler() override;
    io_uring_scheduler(io_uring_scheduler const&)            = delete;
    io_uring_scheduler& operator=(io_uring_scheduler const&) = delete;

    void shutdown() override;

    // scheduler virtuals — definitions in Task 6
    void post(std::coroutine_handle<>) const override;
    void post(scheduler_op*) const override;

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

    /** Return the underlying liburing ring.

        Triggers lazy ring initialisation on first call. Used by
        socket op submission helpers (e.g. `io_uring_submit_op`) and
        any other code path that needs a live ring pointer.
    */
    struct ::io_uring* ring() noexcept
    {
        lazy_init_ring();
        return &ring_;
    }

    /// Return the dispatch mutex (protects completed_ops_ / cond_).
    mutex_type& dispatch_mutex() const noexcept { return dispatch_mutex_; }

    /// Return the ring mutex (serialises userspace SQ/CQ access).
    mutex_type& ring_mutex() const noexcept { return ring_mutex_; }

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
    /// Caller MUST hold ring_mutex_ — the flag is plain bool, not atomic,
    /// and the mutex provides the read-modify-write atomicity.
    bool submit_op_posted_exchange(bool desired) const noexcept
    {
        bool prev = submit_op_posted_;
        submit_op_posted_ = desired;
        return prev;
    }

    /// Return a reference to the mutable embedded submit_sqes_op.
    scheduler_op& submit_op_ref() const noexcept
    {
        return submit_op_;
    }

    /// Increment the io_uring in-flight counter. Callers prep an SQE
    /// whose CQE will require IORING_ENTER_GETEVENTS to surface under
    /// DEFER_TASKRUN. Excluded: the wakeup-eventfd multishot SQE, whose
    /// progress doesn't depend on userspace getevents.
    void inflight_inc() const noexcept
    {
        io_uring_inflight_.fetch_add(1, std::memory_order_release);
    }

    /// Initialize the io_uring ring on first access. Idempotent.
    void lazy_init_ring() const;

    /// Wake the leader if it's blocked in `submit_and_wait_timeout`.
    /// Best-effort: the wakeup is suppressed if the leader has already
    /// been signalled and not yet acked.
    void interrupt_reactor() const noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` targeting an in-flight op by its
        user_data pointer.

        The kernel delivers `-ECANCELED` on the target's CQE if it was
        still in flight; the op's completion handler then reports
        `operation_aborted`.  Best-effort: if the SQ is full after one
        flush attempt the function returns without cancelling (the op
        will complete normally on its own).

        @param target The in-flight op to cancel.
    */
    void submit_cancel_by_user_data(io_uring_op* target) noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` with `IORING_ASYNC_CANCEL_FD`
        to cancel every in-flight op on the given fd in one SQE.

        Best-effort: if the SQ is full after one flush attempt the
        function returns without cancelling.

        @param fd The file descriptor whose in-flight ops should be
            cancelled.
    */
    void submit_cancel_by_fd(int fd) noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` for `fd` and immediately flush
        the submission ring to the kernel.

        Must be called while `fd` is still open so the kernel can
        resolve the file from the fd number before it is closed and
        potentially recycled.

        Best-effort: if the SQ is full the function still flushes any
        earlier pending SQEs to the kernel.

        @param fd The file descriptor whose in-flight ops should be
            cancelled.
    */
    void cancel_and_flush(int fd) noexcept;

    /** Drain pending CQEs for a specific op's `user_data`.

        Submits an ASYNC_CANCEL by user_data to short-circuit any
        in-flight op holding `target`, then iterates the CQ ring and
        consumes every CQE matching `target` so its memory can be
        freed safely. Used by member-owned ops (e.g.
        `uring_multi_accept_op`) whose destructor cannot tolerate
        outstanding CQEs.

        @par Thread Safety
        Safe to call from any thread. Internally takes `ring_mutex_`
        to serialise against the run-loop leader; calls
        `interrupt_reactor()` first so the leader returns from its
        kernel wait promptly.

        @param target The op pointer used as user_data on the SQE.
    */
    void drain_cqes_for(io_uring_op* target) noexcept;

    /** Queue an already-counted op while the caller holds dispatch_mutex_.

        Does NOT increment `outstanding_work_`. Use for synchronous
        completion paths (e.g. SQE backpressure) where the caller called
        `work_started()` and already holds the dispatch lock.

        @pre `dispatch_mutex_` must be locked by the calling thread.
    */
    void push_completed_locked(scheduler_op* op) const noexcept
    {
        completed_ops_.push(op);
    }

    /// Single-threaded mode toggle (matches reactor_scheduler API).
    void configure_single_threaded(bool v) noexcept override
    {
        single_threaded_ = v;
        dispatch_mutex_.set_enabled(!v);
        ring_mutex_.set_enabled(!v);
        cond_.set_enabled(!v);
    }

    /** Configure SQPOLL parameters.

        Must be called before the first run/poll/post — the values
        are cached and read by `lazy_init_ring_unlocked` when the
        ring is first constructed. No-op if `enable` is false (the
        default).

        @note  When combined with single-threaded mode,
        IORING_SETUP_DEFER_TASKRUN is suppressed — the kernel
        rejects that combination. SINGLE_ISSUER still applies.

        @param enable    Set IORING_SETUP_SQPOLL on ring init.
        @param idle_ms   sq_thread_idle in milliseconds; 0 = kernel
                         default (1ms).
        @param cpu       Pin the polling thread to this CPU; -1 to
                         not pin.
    */
    void configure_sqpoll(
        bool enable, unsigned idle_ms, int cpu) noexcept
    {
        enable_sqpoll_     = enable;
        sq_thread_idle_ms_ = idle_ms;
        sq_thread_cpu_     = cpu;
    }

    /// Return true if single-threaded (lockless) mode is active.
    bool is_single_threaded() const noexcept override { return single_threaded_; }

private:
    // ring_ + wakeup_eventfd_ are mutable so lazy_init_ring() (called
    // from const contexts like post()) can populate them on first use.
    mutable struct ::io_uring          ring_{};
    mutable int                       wakeup_eventfd_ = -1;
    timer_service*                    timer_svc_      = nullptr;

    // dispatch_mutex_ protects completed_ops_, cond_, task_running_.
    // ring_mutex_ protects every userspace touch of ring_ (SQ tail,
    // CQ head): get_sqe / submit / submit_and_wait_timeout /
    // for_each_cqe / cq_advance.
    //
    // process_completions runs under ring_mutex_ and briefly takes
    // dispatch_mutex_ to splice into completed_ops_. The locks are
    // never held simultaneously for the full duration of any other
    // path's critical section, so no deadlock.
    mutable mutex_type                dispatch_mutex_{true};
    mutable mutex_type                ring_mutex_{true};
    mutable event_type                cond_{true};
    mutable op_queue                  completed_ops_;
    // outstanding_work_ and io_uring_inflight_ are both atomic
    // counters updated at high frequency on different paths:
    //   - outstanding_work_ : every work_started / work_finished call,
    //                         including timers, posts, and SQE submits.
    //   - io_uring_inflight_ : only SQE submit + non-F_MORE CQE consume.
    // Under multi-thread workloads the threads tend to update these
    // from different code paths; placing them on the same cache line
    // would cause false sharing and unnecessary cache-line ping-pong.
    // Hold each on its own line.
    alignas(64) mutable std::atomic<std::int64_t> outstanding_work_{0};
    // Count of io_uring SQEs in flight whose completion requires user-
    // space to enter the kernel via IORING_ENTER_GETEVENTS for task
    // work to progress under IORING_SETUP_DEFER_TASKRUN. Excludes the
    // wakeup-eventfd multishot poll (registered in lazy_init_ring), and
    // is updated by io_uring_submit_op and by process_completions on
    // each non-F_MORE, non-eventfd CQE. Used by do_one to skip the
    // ring pump when there is no io_uring work pending.
    alignas(64) mutable std::atomic<std::int64_t> io_uring_inflight_{0};
    std::atomic<bool>                 stopped_{false};
    // Leader-follower flag: true while a thread is blocked in
    // io_uring_submit_and_wait_timeout. Protected by dispatch_mutex_.
    mutable bool                      task_running_   = false;
    bool                              single_threaded_ = false;
    bool                              enable_sqpoll_     = false;
    unsigned                          sq_thread_idle_ms_ = 0;
    int                               sq_thread_cpu_     = -1;

    int                               cancel_sentinel_ = 0;
    mutable std::atomic<bool>         wakeup_armed_{false};

    /// Flushes the SQ ring and drains CQEs in one mutex-held pass.
    /// One instance covers a whole batch; subsequent SQEs in the same
    /// batch skip the post, amortising syscall cost across the batch.
    /// Mirrors Asio's `submit_sqes_op` (`io_uring_service.ipp:730-742`).
    struct submit_sqes_op final : scheduler_op
    {
        io_uring_scheduler* sched_ = nullptr;

        submit_sqes_op() noexcept : scheduler_op(&do_handler) {}

        static void do_handler(
            void* owner, scheduler_op* base,
            std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept;
    };

    /// True between the first submitter of a batch posting `submit_op_`
    /// and the dispatched op clearing the flag inside its handler. Read
    /// and written only while holding `ring_mutex_`.
    mutable bool                      submit_op_posted_ = false;

    /// Single embedded `submit_sqes_op` instance, owned by the scheduler.
    mutable submit_sqes_op            submit_op_;

    // drain_cqes_for tuning. The bound exists to avoid stalling a
    // destructor if the kernel never returns a cancel completion (best-
    // effort drain); 8 rounds * 1ms == 8ms worst case.
    static constexpr int              drain_cqes_max_rounds = 8;
    static constexpr unsigned long    drain_cqes_kick_ns    = 1'000'000;

    // ring_inited_ goes true once on first run/poll/submit. The init is
    // deferred from the constructor so configure_single_threaded(true)
    // can take effect before io_uring_queue_init_params chooses flags.
    mutable std::once_flag            ring_init_once_;
    mutable bool                      ring_inited_ = false;

    std::size_t do_one(long timeout_us);
    void        process_completions();
    void        drain_wakeup_eventfd() const noexcept;
    void        lazy_init_ring_unlocked() const;
};

inline
io_uring_scheduler::io_uring_scheduler(
    capy::execution_context& ctx, int /*concurrency_hint*/)
{
    // sched_ cannot be set in the member initialiser — `this` is not
    // available there.
    submit_op_.sched_ = this;

    // Wire timer service. on_earliest_changed wakes the run loop so it
    // recomputes its wait timeout.
    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(this, [](void* p) {
            static_cast<io_uring_scheduler*>(p)->interrupt_reactor();
        }));

    get_resolver_service(ctx, *this);
    get_signal_service(ctx, *this);

    // Ring init is deferred to lazy_init_ring() so configure_single_-
    // threaded(true), which the io_context applies after construction,
    // can take effect before io_uring_queue_init_params chooses flags.
}

inline
io_uring_scheduler::~io_uring_scheduler()
{
    if (ring_inited_)
    {
        if (wakeup_eventfd_ >= 0)
            ::close(wakeup_eventfd_);
        ::io_uring_queue_exit(&ring_);
    }
}

inline void
io_uring_scheduler::lazy_init_ring() const
{
    std::call_once(ring_init_once_, [this] {
        lazy_init_ring_unlocked();
    });
}

inline void
io_uring_scheduler::lazy_init_ring_unlocked() const
{
    io_uring_params params{};
    if (single_threaded_)
    {
        // SINGLE_ISSUER promises the kernel one submitter thread,
        // letting it skip internal SQ locking. DEFER_TASKRUN tells
        // it to batch task_work delivery at io_uring_enter(GETEVENTS)
        // boundaries instead of interrupting the run thread via
        // TWA_SIGNAL — eliminates cache pollution from mid-flight
        // task_work and gives a meaningful single-threaded
        // throughput uplift.
        //
        // Plan 3 disabled DEFER_TASKRUN defensively over a misread
        // of the GETEVENTS contract. Plan 4a re-enabled it: liburing's
        // io_uring_submit_and_wait_timeout always sets
        // IORING_ENTER_GETEVENTS when wait_nr > 0, regardless of
        // ts. Our run loop's only kernel-wait call passes wait_nr=1.
        // Submit-only paths (cancel_and_flush, etc.) leave their
        // CQEs queued until the leader's next GETEVENTS-bearing
        // wait — benign.
        //
        // Multi-thread mode never sets these flags: SINGLE_ISSUER
        // would be unsafe with multiple submitter threads.
        //
        // DEFER_TASKRUN is suppressed when SQPOLL is also enabled
        // — the kernel rejects that combination with -EINVAL. The
        // SQPOLL polling thread already delivers completions
        // without TWA_SIGNAL interruption, so DEFER_TASKRUN's
        // benefit is moot in that mode.
        params.flags = IORING_SETUP_SINGLE_ISSUER;
        if (!enable_sqpoll_)
            params.flags |= IORING_SETUP_DEFER_TASKRUN;
    }

    if (enable_sqpoll_)
    {
        // SQPOLL forks a kernel thread that busy-polls the SQ ring;
        // submission becomes a userspace-only memory store. Combines
        // with SINGLE_ISSUER (the kernel accepts that pair) but NOT
        // with DEFER_TASKRUN (kernel returns -EINVAL); the
        // single_threaded_ branch above suppresses DEFER_TASKRUN
        // when SQPOLL is also set. Idle timeout 0 means kernel
        // default (1ms); we only forward when explicitly set so
        // the kernel default is preserved.
        params.flags |= IORING_SETUP_SQPOLL;
        if (sq_thread_idle_ms_ != 0)
            params.sq_thread_idle = sq_thread_idle_ms_;
        if (sq_thread_cpu_ >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = static_cast<__u32>(sq_thread_cpu_);
        }
    }

    int rc = ::io_uring_queue_init_params(256, &ring_, &params);
    if (rc < 0)
        detail::throw_system_error(
            make_err(-rc), "io_uring_queue_init_params");

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
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(
            make_err(-submit_rc), "io_uring_submit (wakeup)");
    }

    ring_inited_ = true;
}

inline void
io_uring_scheduler::shutdown()
{
    stopped_.store(true, std::memory_order_release);

    // Drain posted ops, calling destroy() on each so embedded handles
    // (coroutine frames, error_code outputs) get torn down rather
    // than leaked. Mirrors reactor_scheduler::shutdown_drain.
    //
    // Service shutdown order (driven by capy::execution_context):
    // each socket/acceptor service::shutdown() submits a cancel SQE
    // for every live impl. The CQEs that result either land in
    // completed_ops_ (drained here as op->destroy()) or stay in the
    // kernel ring; ~scheduler's io_uring_queue_exit cleans the
    // latter up at process teardown. Self-referential impl_ptr
    // cycles (e.g. multishot acceptor's multi_op_->impl_ptr) are
    // broken explicitly inside each service before the scheduler
    // shutdown runs.
    lock_type lock(dispatch_mutex_);
    while (auto* op = completed_ops_.pop())
    {
        lock.unlock();
        op->destroy();
        lock.lock();
    }
    cond_.notify_all();
}

inline void
io_uring_scheduler::stop()
{
    stopped_.store(true, std::memory_order_release);
    {
        lock_type lock(dispatch_mutex_);
        cond_.notify_all();
    }
    // Force-wake unconditionally — bypass interrupt_reactor's CAS
    // coalescing. A dropped wake here leaves the leader blocked
    // forever in submit_and_wait_timeout (no further CQE will
    // arrive after stop()). With multishot poll on wakeup_eventfd_,
    // this write reliably produces a CQE.
    if (ring_inited_)
    {
        std::uint64_t v = 1;
        [[maybe_unused]] auto r =
            ::write(wakeup_eventfd_, &v, sizeof(v));
    }
}

inline bool
io_uring_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

inline void
io_uring_scheduler::restart()
{
    stopped_.store(false, std::memory_order_release);
}

inline void
io_uring_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
io_uring_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

inline void
io_uring_scheduler::interrupt_reactor() const noexcept
{
    // Skip if the ring hasn't been initialised yet — there's no leader
    // to wake and no eventfd to write.
    if (!ring_inited_)
        return;

    // Single-thread: the user's coroutines run on the leader thread,
    // so when interrupt_reactor is called from user code the leader
    // is not in kernel wait — there is nothing to wake.
    if (single_threaded_)
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
    [[maybe_unused]] auto r = ::write(wakeup_eventfd_, &v, sizeof(v));
    wakeup_armed_.store(true, std::memory_order_release);
}

inline void
io_uring_scheduler::drain_wakeup_eventfd() const noexcept
{
    std::uint64_t v;
    [[maybe_unused]] auto r = ::read(wakeup_eventfd_, &v, sizeof(v));

    // Multishot poll never needs re-arming. The poll-add was queued
    // once at lazy_init_ring with IORING_POLL_ADD_MULTI; each eventfd
    // POLLIN produces a CQE without consuming the SQE.
    //
    // Release pairs with the acquire side of interrupt_reactor's CAS:
    // a posting thread that observes wakeup_armed_ == false from this
    // store will see the eventfd already drained by the leader.
    wakeup_armed_.store(false, std::memory_order_release);
}

inline void
io_uring_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;
        explicit post_handler(std::coroutine_handle<> h) noexcept : h_(h) {}

        void operator()() override
        {
            auto saved = h_;
            delete this;
            std::atomic_thread_fence(std::memory_order_acquire);
            saved.resume();
        }

        void destroy() override
        {
            auto saved = h_;
            delete this;
            if (saved)
                saved.destroy();
        }
    };

    auto* op = new post_handler(h);
    lazy_init_ring();
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    bool wake_leader;
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.push(op);
        wake_leader = task_running_;
        if (!wake_leader)
            cond_.notify_one();
    }
    if (wake_leader)
        interrupt_reactor();
}

inline void
io_uring_scheduler::post(scheduler_op* op) const
{
    lazy_init_ring();
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
    bool wake_leader;
    {
        lock_type lock(dispatch_mutex_);
        completed_ops_.push(op);
        wake_leader = task_running_;
        if (!wake_leader)
            cond_.notify_one();
    }
    if (wake_leader)
        interrupt_reactor();
}

// Thread-local stack of frames for io_uring schedulers being run on the
// current thread. Holds the running-scheduler pointer (for
// running_in_this_thread reporting) and the inline completion budget
// used by the speculative non-blocking I/O path (plan 5j). Nesting
// stacks frames via prev_ so each scheduler gets its own budget.
struct io_uring_scheduler_frame
{
    io_uring_scheduler const* sched;
    io_uring_scheduler_frame* prev;
    int                       inline_budget;
    int                       inline_budget_max;
};

inline thread_local io_uring_scheduler_frame* tl_running_scheduler_frame_ = nullptr;

// Default inline budget. Matches reactor's initial budget (2). Adaptive
// ramp-up to a max is intentionally NOT implemented yet — keep it simple
// for plan 5j and revisit if benches show fairness issues.
inline constexpr int io_uring_inline_budget_initial = 2;
inline constexpr int io_uring_inline_budget_max     = 16;

/// RAII guard: pushes a frame onto the thread's running-scheduler stack
/// on construction, restores the previous on destruction. Used by
/// run/run_one/wait_one/poll/poll_one to mark the running thread and
/// hold a fresh inline budget for speculative completions.
struct io_uring_run_guard
{
    io_uring_scheduler_frame frame_;

    explicit io_uring_run_guard(io_uring_scheduler const* self) noexcept
        : frame_{self, tl_running_scheduler_frame_,
                 io_uring_inline_budget_initial,
                 io_uring_inline_budget_max}
    {
        tl_running_scheduler_frame_ = &frame_;
    }

    ~io_uring_run_guard() noexcept
    {
        tl_running_scheduler_frame_ = frame_.prev;
    }
};

inline bool
io_uring_scheduler::running_in_this_thread() const noexcept
{
    for (auto* f = tl_running_scheduler_frame_; f != nullptr; f = f->prev)
    {
        if (f->sched == this)
            return true;
    }
    return false;
}

inline void
io_uring_scheduler::reset_inline_budget() const noexcept
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
io_uring_scheduler::try_consume_inline_budget() const noexcept
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
io_uring_scheduler::run()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    io_uring_run_guard guard(this);
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
io_uring_scheduler::run_one()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    io_uring_run_guard guard(this);
    return do_one(-1);
}

inline std::size_t
io_uring_scheduler::wait_one(long usec)
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    io_uring_run_guard guard(this);
    return do_one(usec);
}

inline std::size_t
io_uring_scheduler::poll()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    io_uring_run_guard guard(this);
    std::size_t n = 0;
    while (do_one(0))
    {
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    }
    return n;
}

inline std::size_t
io_uring_scheduler::poll_one()
{
    lazy_init_ring();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }
    io_uring_run_guard guard(this);
    return do_one(0);
}

inline std::size_t
io_uring_scheduler::do_one(long timeout_us)
{
    // Leader-follower: only one thread at a time may call
    // io_uring_submit_and_wait_timeout on a shared ring (liburing's
    // userspace head/tail bookkeeping is not thread-safe). Other
    // threads either dispatch ready ops from completed_ops_ or wait
    // on cond_ until the leader returns from the kernel.
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    // submit_sqes_op only pumps the ring once per SQE batch. If the user
    // keeps a non-empty completed_ops_ (e.g. timer with 0ns expiry as a
    // yield primitive), the leader-phase kernel pass below never runs
    // and CQEs accumulate in the ring forever — sub_request's read CQE
    // never gets drained and the bench spins. submit_and_get_events
    // (not plain submit) is required because IORING_SETUP_DEFER_TASKRUN
    // gates task work on IORING_ENTER_GETEVENTS.
    //
    // Gate the kernel pump on there being io_uring-specific work. The
    // check is performed under ring_mutex_ so a concurrent cross-thread
    // submitter cannot prep an SQE that we then race past — both this
    // path and io_uring_submit_op acquire ring_mutex_ before touching
    // the ring. When all three sources are empty (no io_uring ops in
    // flight needing DEFER_TASKRUN GETEVENTS, no userspace-pending
    // SQEs, no kernel-ready CQEs) a kernel entry would have no work —
    // saves ~8 pp of cycles on the no-I/O microbenchmark
    // (io_context:single_threaded). We deliberately do NOT include
    // outstanding_work_ here, because that counter mixes coroutine
    // posts (in completed_ops_) with io_uring work — IOCTX has many
    // coroutine posts and no io_uring work, and the kernel pump there
    // is pure overhead.
    if (ring_inited_)
    {
        lock_type ring_lock(ring_mutex_);
        if (io_uring_inflight_.load(std::memory_order_acquire) != 0
            || ::io_uring_sq_ready(&ring_) != 0
            || ::io_uring_cq_ready(&ring_) != 0)
        {
            ::io_uring_submit_and_get_events(&ring_);
            process_completions();
        }
    }

    // Drain expired timers eagerly, for the same reason the kernel CQE
    // pump runs unconditionally above: when completed_ops_ stays non-
    // empty (e.g. continuous loopback I/O whose CQEs land in the top-
    // of-do_one process_completions call), the leader-wait branch
    // below — the only other place process_expired() runs — is never
    // reached. Without this, stopper-timer-based shutdowns (and any
    // other timer dependent on a busy I/O loop yielding) deadlock.
    //
    // empty() is a single relaxed-acquire atomic load on
    // timer_service::cached_nearest_ns_ (lock-free, no clock_gettime).
    // Skipping process_expired() when no timer is registered avoids the
    // mutex + clock_gettime hot-path cost that dominates IOCTX cycles
    // (~25 pp on io_context:single_threaded). When a timer IS
    // registered the call runs exactly as before, preserving the
    // deadlock fix this guard was originally written to address.
    if (!timer_svc_->empty())
        timer_svc_->process_expired();

    lock_type lock(dispatch_mutex_);
    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        if (auto* op = completed_ops_.pop())
        {
            // Hand off any remaining queued work to a follower so we
            // dispatch in parallel.
            if (!completed_ops_.empty())
                cond_.notify_one();
            lock.unlock();
            // Speculative follow-ups in the handler share this budget.
            reset_inline_budget();
            (*op)();
            work_finished();
            return 1;
        }

        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        if (task_running_)
        {
            // Another thread holds leadership; either return (poll)
            // or wait for it to deliver work / release leadership.
            if (timeout_us == 0)
                return 0;
            if (timeout_us < 0)
                cond_.wait(lock);
            else
            {
                cond_.wait_for(
                    lock, std::chrono::microseconds(timeout_us));
                // wait_one honoured its timeout; if nothing arrived,
                // return rather than re-arm.
                if (completed_ops_.empty() &&
                    !stopped_.load(std::memory_order_acquire))
                    return 0;
            }
            continue;
        }

        // Become the leader: run the kernel poll. We drop the lock
        // for the blocking wait, then take it back to release
        // leadership and wake any follower that should pick up new
        // work.
        __kernel_timespec  ts{};
        __kernel_timespec* ts_ptr      = nullptr;
        auto               next_expiry = timer_svc_->nearest_expiry();
        auto               now = std::chrono::steady_clock::now();

        if (timeout_us == 0)
        {
            ts.tv_sec  = 0;
            ts.tv_nsec = 0;
            ts_ptr     = &ts;
        }
        else if (next_expiry != timer_service::time_point::max())
        {
            auto delta_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    next_expiry - now)
                    .count();
            if (delta_ns < 0) delta_ns = 0;
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

        task_running_ = true;
        lock.unlock();

        // Three-phase kernel wait, matching Boost.Asio's
        // io_uring_service::run pattern. ring_mutex_ is held briefly
        // to push pending SQEs and to drain CQEs, but NOT during
        // the blocking io_uring_wait_cqe_timeout. Cross-thread
        // submitters (io_uring_submit_op, cancel paths) can take
        // ring_mutex_ during the wait and prep new SQEs without
        // blocking on the leader; their wake eventfd write fires the
        // multishot poll and returns the leader from wait_cqe_timeout
        // promptly.
        //
        // Phase 1 — submit any pending SQEs to the kernel.
        {
            lock_type ring_lock(ring_mutex_);
            ::io_uring_submit(&ring_);
        }

        // Phase 2 — wait for at least one CQE without holding the
        // mutex. Multi-thread `io_uring_enter` is permitted without
        // SINGLE_ISSUER. wait_cqe_timeout only peeks the CQ ring;
        // head advancement happens under the mutex in
        // process_completions below.
        ::io_uring_cqe* cqe = nullptr;
        int rc = ::io_uring_wait_cqe_timeout(&ring_, &cqe, ts_ptr);

        // Phase 3 — drain CQEs under the mutex.
        {
            lock_type ring_lock(ring_mutex_);
            if (rc == 0 || rc == -ETIME || rc == -EINTR)
                process_completions();
        }

        if (rc < 0 && rc != -ETIME && rc != -EINTR)
        {
            // Restore state before propagating so followers don't
            // deadlock waiting for a leader that never returns.
            lock.lock();
            task_running_ = false;
            cond_.notify_all();
            detail::throw_system_error(
                make_err(-rc), "io_uring_wait_cqe_timeout");
        }

        if (!timer_svc_->empty())
            timer_svc_->process_expired();

        lock.lock();
        task_running_ = false;
        cond_.notify_all();

        // For poll() / wait_one() we honour the timeout: one kernel
        // pass is the contract. If still nothing dispatchable, exit.
        // For run() (timeout < 0) keep looping until work arrives or
        // someone calls stop().
        if (timeout_us >= 0 && completed_ops_.empty())
            return 0;
    }
}

inline void
io_uring_scheduler::process_completions()
{
    unsigned head;
    ::io_uring_cqe* cqe;
    unsigned consumed = 0;

    // Collect completed I/O ops locally; splice into completed_ops_
    // after the loop so do_one dispatches them one at a time.
    op_queue local_ops;

    std::int64_t inflight_dec = 0;
    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        void* ud = io_uring_cqe_get_data(cqe);
        if (ud == nullptr)
        {
            // Wakeup eventfd CQE: drain the eventfd byte. Not counted
            // by io_uring_inflight_; we never incremented for the
            // wakeup multishot SQE (its progress doesn't depend on
            // userspace getevents).
            drain_wakeup_eventfd();
            // If multishot terminated (kernel dropped under memory
            // pressure or similar), re-arm. Each CQE except the last
            // sets IORING_CQE_F_MORE.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
            {
                ::io_uring_sqe* re = ::io_uring_get_sqe(&ring_);
                if (!re)
                {
                    ::io_uring_submit(&ring_);
                    re = ::io_uring_get_sqe(&ring_);
                }
                if (re)
                {
                    ::io_uring_prep_poll_multishot(
                        re, wakeup_eventfd_, POLLIN);
                    ::io_uring_sqe_set_data(re, nullptr);
                }
            }
        }
        else if (ud == &cancel_sentinel_)
        {
            // CQE for an ASYNC_CANCEL op — ignore; the actual op's
            // CQE arrives separately and is dispatched via cqe_func.
            // Cancels are one-shot, no F_MORE, decrement inflight.
            ++inflight_dec;
        }
        else
        {
            auto* iop = static_cast<io_uring_op*>(ud);
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
        io_uring_inflight_.fetch_sub(
            inflight_dec, std::memory_order_acq_rel);

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
io_uring_scheduler::submit_sqes_op::do_handler(
    void* owner, scheduler_op* base,
    std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
{
    if (owner == nullptr)
        return;   // shutdown drain — nothing to do; SQE storage is
                  // kernel-mapped and discarded by io_uring_queue_exit.

    auto* self  = static_cast<submit_sqes_op*>(base);
    auto* sched = self->sched_;

    io_uring_scheduler::lock_type ring_lock(sched->ring_mutex_);
    sched->submit_op_posted_ = false;
    ::io_uring_submit_and_get_events(&sched->ring_);
    sched->process_completions();
}

inline void
io_uring_scheduler::submit_cancel_by_user_data(io_uring_op* target) noexcept
{
    lazy_init_ring();
    // Wake the leader (if any) so its submit_and_wait_timeout returns
    // and releases ring_mutex_; otherwise we'd block here until the
    // next CQE arrives organically. Cancellation is best-effort if
    // the SQ stays full after one flush — the op completes on its
    // own and reports cancelled via the in-flight `cancelled` flag.
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return;

    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, &cancel_sentinel_);
    inflight_inc();
}

inline void
io_uring_scheduler::submit_cancel_by_fd(int fd) noexcept
{
    lazy_init_ring();
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return;

    io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data(sqe, &cancel_sentinel_);
    inflight_inc();
}

inline void
io_uring_op::request_cancel() noexcept
{
    cancelled.store(true, std::memory_order_release);
    // Skip the cancel SQE if we never linked an SQE to this op — the
    // bypass path in the caller will see cancelled=true and complete
    // synchronously without a kernel round-trip.
    if (sched_ && sqe_set.load(std::memory_order_acquire))
        sched_->submit_cancel_by_user_data(this);
}

inline void
io_uring_scheduler::cancel_and_flush(int fd) noexcept
{
    lazy_init_ring();
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (sqe)
    {
        io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data(sqe, &cancel_sentinel_);
        inflight_inc();
    }
    // Flush while fd is still open so the kernel resolves the file
    // from the fd number before the caller closes and recycles it.
    io_uring_submit(&ring_);
}

inline void
io_uring_scheduler::drain_cqes_for(io_uring_op* target) noexcept
{
    lazy_init_ring();
    // Submit a cancel by user_data so the kernel returns CQEs for
    // the target promptly, then iterate the CQ ring and consume
    // every CQE that matches `target`. ring_mutex_ serializes against
    // the leader's kernel wait and any concurrent cancel path; the
    // interrupt_reactor() ensures the leader returns promptly so we
    // can take the mutex.
    interrupt_reactor();
    {
        lock_type lock(ring_mutex_);
        if (auto* sqe = io_uring_get_sqe(&ring_))
        {
            io_uring_prep_cancel(sqe, target, 0);
            io_uring_sqe_set_data(sqe, &cancel_sentinel_);
            inflight_inc();
        }
        io_uring_submit(&ring_);
    }

    // Loop a few rounds: cancel SQE submission, then drain CQEs.
    // Bounded loop avoids stalls if the kernel never returns a
    // cancel completion — best-effort.
    for (int rounds = 0; rounds < drain_cqes_max_rounds; ++rounds)
    {
        lock_type lock(ring_mutex_);

        unsigned        head;
        ::io_uring_cqe* cqe;
        unsigned        consumed = 0;
        bool            saw_target = false;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            void* ud = io_uring_cqe_get_data(cqe);
            if (ud == target)
            {
                saw_target = true;
                // Don't dispatch — caller is destructing target;
                // just consume so the CQE doesn't dangle.
            }
            // Other CQEs are intentionally NOT dispatched here. They
            // may belong to ops freed by sibling teardowns (other
            // acceptors / sockets), and dispatching would UAF. The
            // next normal run-loop iteration will handle them; the
            // io_context's destructor sequence runs services'
            // shutdowns before ~scheduler so any still-live ops get
            // a chance to drain through their own paths first.
            ++consumed;
        }
        if (consumed)
        {
            io_uring_cq_advance(&ring_, consumed);
            if (saw_target)
                break;
            continue;
        }

        // Nothing in the CQ — kick the kernel briefly. Hold
        // ring_mutex_ across the wait so we don't race with the
        // run-loop leader.
        __kernel_timespec ts{
            0, static_cast<long long>(drain_cqes_kick_ns)};
        ::io_uring_cqe* one = nullptr;
        int rc = ::io_uring_submit_and_wait_timeout(
            &ring_, &one, 1, &ts, nullptr);
        if (rc < 0 && rc != -ETIME && rc != -EINTR)
            break;
        if (rc == -ETIME)
            break;
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP
