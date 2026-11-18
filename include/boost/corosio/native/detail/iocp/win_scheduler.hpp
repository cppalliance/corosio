//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/detail/scheduler.hpp>
#include <system_error>

#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/ready_queue.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/abandon.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>

#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_timers.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

// Forward declarations
struct overlapped_op;
class win_timers;
class win_wait_reactor;

namespace iocp {
struct scheduler_context;
struct thread_context_guard;
} // namespace iocp

class BOOST_COROSIO_DECL win_scheduler final
    : public scheduler
{
public:

    win_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);
    ~win_scheduler();
    win_scheduler(win_scheduler const&)            = delete;
    win_scheduler& operator=(win_scheduler const&) = delete;

    void shutdown() override;
    void post(std::coroutine_handle<> h) const override;
    void post(scheduler_op* h) const override;
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

    void* native_handle() const noexcept
    {
        return iocp_;
    }

    void work_started() noexcept override;
    void work_finished() noexcept override;

    // IOCP has only the dispatch mutex; reactor_io_locking and one_thread do
    // not apply (the completion port provides its own synchronization).
    void configure_threading(threading_config cfg) noexcept override
    {
        scheduler_locking_disabled_ = !cfg.scheduler_locking;
        one_thread_                 = cfg.one_thread;
        dispatch_mutex_.set_enabled(cfg.scheduler_locking);
    }

    /// Return true when scheduler locking is disabled (fully-lockless tier).
    bool scheduler_locking_disabled() const noexcept override
    {
        // LCOV_EXCL_START: consulted only by the POSIX pool-backed
        // services; the IOCP services do not read it yet.
        return scheduler_locking_disabled_;
        // LCOV_EXCL_STOP
    }

    /** Signal that an overlapped I/O operation is now pending.
        Coordinates with the run loop via the ready_ CAS protocol. */
    void on_pending(overlapped_op* op) const;

    /** Post an immediate completion with pre-stored results.
        Used for sync errors and noop paths. */
    void on_completion(overlapped_op* op, DWORD error, DWORD bytes) const;

    // Timer service integration
    void update_timeout();

    /** Set how many operations a handler may complete inline.

        An operation the kernel finishes during the initiating call can
        resume its coroutine at once instead of going through the run
        loop. The budget bounds how many do so in a row before the
        loop gets to run other work; zero sends every completion
        through the loop.
    */
    void configure_inline_budget(unsigned max) noexcept
    {
        constexpr auto cap =
            static_cast<unsigned>((std::numeric_limits<int>::max)());
        inline_budget_max_ = static_cast<int>(max < cap ? max : cap);
    }

    /** Take one unit of the running handler's inline budget.

        @return True if the caller, on a thread running this scheduler,
            may complete its operation inline.
    */
    bool try_consume_inline_budget() const noexcept;

    /** Have the calling loop thread look at the port before it runs
        more of its own queue.

        A thread with work of its own queued visits the port only now
        and then. An operation that wants the completions already
        waiting there to be taken first asks for the visit.
    */
    void request_port_visit() const noexcept;

private:
    friend struct iocp::thread_context_guard;

    // The calling thread's frame when work it produces can stay off
    // the port: the innermost run loop on this thread is this
    // scheduler's and the only one running, it is not a
    // single-handler call, and the scheduler is not stopped.
    iocp::scheduler_context* local_context() const noexcept;

    // Queue work a loop thread produced without a packet: on the
    // thread's own queue when a single run thread is guaranteed, on
    // the shared queue otherwise.
    template<class Entry>
    void keep(iocp::scheduler_context& t, Entry&& e) const noexcept;

    // The same for a completion whose work the caller counted.
    void
    keep_counted(iocp::scheduler_context& t, overlapped_op* op) const noexcept;

    // Add the frame's unpublished work count to the shared one,
    // stopping the scheduler if that leaves no work.
    void publish_work(iocp::scheduler_context& t) const noexcept;

    // Publish the frame's work count and move what it still holds to
    // the shared queue, for another loop or a later run call.
    void flush_context(iocp::scheduler_context& t) const noexcept;

    // Wake a loop blocked in the port for the shared queue, if there
    // is one. Call after setting dispatch_queue.
    void share_queue_wake() const noexcept;

    // Sort one batch of dequeued packets into the frame's queues.
    void claim(
        iocp::scheduler_context& t,
        ready_queue& into,
        void const* entries,
        unsigned long count,
        bool& took_wake);

    // Destroy a queue entry without running it.
    static void destroy_entry(void* e) noexcept;

    void on_timer_service_attached(timer_service& svc) override;

    /// Destroy every packet and deferred entry still queued, without
    /// running them; stops once the port is empty.
    void sweep_port() noexcept;

    // Posts an allocated copy, which survives the caller and keeps the
    // owner for teardown.
    void post_copy(capy::continuation const& c) const;

    static void on_timer_changed(void* ctx);
    static unsigned long
    timer_timeout_ms(timer_service& svc, unsigned long timeout_ms) noexcept;
    // Set stopped_ and post the shutdown packet; returns the Win32
    // error of a failed post, or 0.
    unsigned long post_stop() const noexcept;

    // Post the shutdown packet unless one is in flight; returns the
    // Win32 error of a failed post, or 0.
    unsigned long post_wakeup() const noexcept;

    // Retry a recorded wakeup that could not be queued, throwing if
    // the retry fails too.
    void report_stop_failure();

    // Record a failed wakeup for the next run or poll call to retry,
    // then throw it.
    [[noreturn]] void throw_wakeup_failure(unsigned long err);
    std::size_t do_one(iocp::scheduler_context& t, unsigned long timeout_ms);
    std::size_t do_one_wait(
        iocp::scheduler_context& t, unsigned long timeout_ms, bool& took_wake);

    void* iocp_;
    int inline_budget_max_ = 16;

    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(
        4251) // std::/detail:: members, dll-interface
    mutable std::atomic<long> outstanding_work_;

    // Packets in flight to the completion port that reference
    // overlapped-op memory: kernel completions owed after a pending
    // submission, plus successful stored-result posts. Shutdown reaps
    // until this is zero before the services free op storage. The
    // run-loop counter cannot serve that role: frames destroyed at
    // teardown never return their work-guard credits.
    mutable std::atomic<long> pending_io_{0};
    mutable std::atomic<long> stopped_;
    mutable std::atomic<long> stop_event_posted_;
    mutable std::atomic<long> stop_failure_{0};
    mutable std::atomic<long> dispatch_required_;

    // Threads with a run-loop frame alive; a loop nested in a handler
    // of the same scheduler is not counted again. A thread keeps work
    // off the port only while it is the one loop there is: with a
    // second, work goes through the port, where the kernel hands it
    // to whichever thread is free first.
    mutable std::atomic<long> loop_threads_{0};

    // Threads about to block, or blocked, in the port with nothing
    // queued. Work put on the shared queue needs a wakeup packet only
    // while there is one.
    mutable std::atomic<long> blocked_{0};

    // A wakeup packet for the shared queue is in the port, so another
    // would wake nobody sooner.
    mutable std::atomic<long> wake_pending_{0};
    bool scheduler_locking_disabled_ = false;

    // A single run thread is guaranteed (a lockless tier). Only then
    // does a thread keep work on its own queue: a second loop could
    // not reach it there behind the handler the first is running.
    bool one_thread_ = false;
    mutable win_mutex dispatch_mutex_;
    mutable ready_queue completed_ops_;
    std::unique_ptr<win_timers> timers_;
    std::unique_ptr<win_wait_reactor> wait_reactor_;
    BOOST_COROSIO_MSVC_WARNING_POP

public:
    /** Return the auxiliary select-based reactor for wait operations.

        Built with the scheduler and stopped+joined in ~win_scheduler,
        so it is always there to hand a wait to. Used by socket and
        acceptor wait() implementations whose readiness cannot be
        expressed natively in IOCP (datagram-read, acceptor-read,
        error-wait).
    */
    win_wait_reactor& wait_reactor();

    /// Cancel a parked wait op. Safe to call from any thread.
    void cancel_wait(overlapped_op* op) noexcept;
};

/*
    ARCHITECTURE NOTE: Function Pointer Dispatch

    All I/O handles are registered with the IOCP using key_io (0).
    Dispatch happens via the function pointer stored in each scheduler_op.

    A dequeued packet's OVERLAPPED* is cast to scheduler_op*, queued
    on the dequeuing thread, and run through the function pointer - no
    virtual dispatch.

    The completion_key enum values are used only for internal signals:
      - key_io (0): Normal I/O completion, dispatch via func_
      - key_wake_dispatch (1): Wakeup for due timers or the shared
        queue, check dispatch_required_
      - key_shutdown (2): Stop signal
      - key_result_stored (3): Results pre-stored in OVERLAPPED
      - key_posted: Carries a scheduler_op* in the OVERLAPPED pointer
      - key_continuation: Carries a capy::continuation* in the OVERLAPPED pointer
*/

namespace iocp {

// Poll interval (ms) for the one-time shutdown drain loop. Unlike the
// steady-state loop (which blocks indefinitely until a real wake-up),
// the drain must keep re-checking the work count and the deferred
// completion queue to make progress, so it uses a finite wait.
inline constexpr unsigned long shutdown_drain_timeout_ms = 500;

// Most packets one dequeue call takes from the port.
inline constexpr unsigned long max_batch = 64;

// Handlers and inline completions a thread runs from its own queue
// before it looks at the port again, so a chain of posts or of
// operations that never pend cannot starve I/O completions.
inline constexpr unsigned poll_every = 32;

/*
    Per-thread state of one run loop.

    While a thread is the one loop running, the work it produces
    stays off the port: handlers posted from a handler, completions
    that never reached the kernel, and the packets of a batched
    dequeue. Queueing it costs no system call.

    Where it is queued depends on what the context promises. A second
    loop may start at any time, while the first is inside a handler,
    and must then be able to run what the first has queued: a handler
    may be waiting for it. So the work goes on the scheduler's shared
    queue, under its lock, and a thread takes one entry from that
    queue at a time. Only a context that guarantees a single run
    thread queues in `ready` instead, which no other thread can see
    and which costs no lock either. `ready` otherwise holds just what
    the thread is about to run: a packet dequeued on its own, and the
    work ahead of a stop.

    `work_delta` is the part of the scheduler's work count this thread
    has not published: +1 for each post to `ready`, -1 for each
    handler run. It is added to the shared count before the thread
    waits on the port, when its queue runs empty or is handed over,
    and when the frame is left. A post to `ready` made from a handler
    leaves the shared count at least one too high until then, because
    the handler that made it, or the one that began the chain, was
    counted there and its -1 is still here; so a work_finished() on
    any thread cannot take the count to zero while this queue holds
    work. Posts the loop makes outside a handler, for expired timers,
    have no such cover and are published at once. A post to the shared
    queue is counted at once: another thread may run it and publish
    its -1 first.
*/
struct BOOST_COROSIO_SYMBOL_VISIBLE scheduler_context
{
    win_scheduler const* key;
    scheduler_context* next;

    // Runnable work owned by this thread.
    ready_queue ready;

    // Packets dequeued in the same batch as a stop, after it. They
    // were queued behind the stop, so they wait for the next run.
    ready_queue after_stop;

    long work_delta     = 0;
    int inline_budget   = 0;
    unsigned since_poll = 0;
    bool stop_seen      = false;

    // False for the calls that run a single handler: what one handler
    // leaves behind would have to be handed back on the way out.
    bool private_ok = true;

    scheduler_context(
        win_scheduler const* k, scheduler_context* n, bool priv) noexcept
        : key(k)
        , next(n)
        , private_ok(priv)
    {
    }
};

inline thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;
    running_scheduler_guard running_;

    // This frame is the thread's outermost on its scheduler, the one
    // counted in loop_threads_.
    bool counted_ = true;

    thread_context_guard(win_scheduler const* ctx, bool private_ok) noexcept;
    ~thread_context_guard() noexcept;
};

} // namespace iocp

/** Return the Win32 error of a completed overlapped request.

    A batched dequeue reports only that packets were taken, so each
    operation's outcome is read from the status the kernel left in its
    `OVERLAPPED`. The conversion is the one `GetQueuedCompletionStatus`
    applies, so the codes are the ones it reports.
*/
inline DWORD
overlapped_error(OVERLAPPED const& ov) noexcept
{
    auto const status = static_cast<LONG>(ov.Internal);
    if (status >= 0)
        return 0;
    using fn_type = ULONG(NTAPI*)(LONG);
    // The two-step cast is the sanctioned FARPROC conversion.
    static fn_type const fn = [] {
        HMODULE h = ::GetModuleHandleW(L"ntdll.dll");
        return h ? reinterpret_cast<fn_type>(reinterpret_cast<void (*)()>(
                       ::GetProcAddress(h, "RtlNtStatusToDosError")))
                 : nullptr;
    }();
    return fn ? fn(status) : static_cast<DWORD>(ERROR_GEN_FAILURE);
}

namespace iocp {

inline thread_context_guard::thread_context_guard(
    win_scheduler const* ctx, bool private_ok) noexcept
    : frame_(ctx, context_stack.get(), private_ok)
    , running_(ctx)
{
    // An outer loop of this scheduler on this thread is suspended
    // inside a handler until this one returns; what it holds must
    // not wait that long.
    for (auto* c = frame_.next; c != nullptr; c = c->next)
    {
        if (c->key == ctx)
        {
            ctx->flush_context(*c);
            counted_ = false;
        }
    }
    if (counted_)
        ctx->loop_threads_.fetch_add(1, std::memory_order_acq_rel);
    context_stack.set(&frame_);
}

inline thread_context_guard::~thread_context_guard() noexcept
{
    context_stack.set(frame_.next);
    frame_.key->flush_context(frame_);
    if (counted_)
        frame_.key->loop_threads_.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace iocp

// The constructor, ~win_scheduler(), shutdown(), wait_reactor() and
// cancel_wait() are defined at the bottom of this header so the
// unique_ptr<win_wait_reactor>'s deleter, its operator* and
// wait_reactor_->stop() see the type complete. The constructor needs
// it to build the reactor, and its unwind path to destroy it.

inline void
win_scheduler::post(std::coroutine_handle<> h) const
{
    post_copy(capy::continuation{h});
}

inline void
win_scheduler::post_copy(capy::continuation const& c) const
{
    struct post_handler final : scheduler_op
    {
        capy::continuation c_;

        static void do_complete(
            void* owner, scheduler_op* base, std::uint32_t, std::uint32_t)
        {
            auto* self = static_cast<post_handler*>(base);
            auto c     = self->c_;
            delete self;
            if (!owner)
            {
                capy::abandon(c);
                return;
            }
            c.h.resume();
        }

        explicit post_handler(capy::continuation const& c)
            : scheduler_op(&do_complete)
            , c_(c)
        {
        }
    };

    post(new post_handler(c));
}

inline void
win_scheduler::post(scheduler_op* h) const
{
    if (auto* t = local_context())
    {
        keep(*t, h);
        return;
    }

    outstanding_work_.fetch_add(1, std::memory_order_acq_rel);

    if (!::PostQueuedCompletionStatus(
            iocp_, 0, key_posted, reinterpret_cast<LPOVERLAPPED>(h)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(h);
        dispatch_required_.fetch_or(
            iocp::dispatch_queue, std::memory_order_seq_cst);
    }
}

inline void
win_scheduler::post(capy::continuation& c) const
{
    if (auto* t = local_context())
    {
        keep(*t, c);
        return;
    }

    outstanding_work_.fetch_add(1, std::memory_order_acq_rel);

    if (!::PostQueuedCompletionStatus(
            iocp_, 0, key_continuation, reinterpret_cast<LPOVERLAPPED>(&c)))
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(c);
        dispatch_required_.fetch_or(
            iocp::dispatch_queue, std::memory_order_seq_cst);
    }
}

template<class Entry>
void
win_scheduler::keep(iocp::scheduler_context& t, Entry&& e) const noexcept
{
    if (one_thread_)
    {
        ++t.work_delta;
        t.ready.push(e);
        return;
    }
    // Counted at once: another loop may run the entry and publish
    // its -1 before this thread publishes anything.
    outstanding_work_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(e);
        dispatch_required_.fetch_or(
            iocp::dispatch_queue, std::memory_order_seq_cst);
    }
    share_queue_wake();
}

inline void
win_scheduler::keep_counted(
    iocp::scheduler_context& t, overlapped_op* op) const noexcept
{
    if (one_thread_)
    {
        t.ready.push(op);
        return;
    }
    {
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        completed_ops_.push(op);
        dispatch_required_.fetch_or(
            iocp::dispatch_queue, std::memory_order_seq_cst);
    }
    share_queue_wake();
}

inline iocp::scheduler_context*
win_scheduler::local_context() const noexcept
{
    auto* t = iocp::context_stack.get();
    // A stopped scheduler takes posts through the port, behind the
    // stop packet, so they wait for the next run as they always have.
    // With another loop running, the port lets whichever thread is
    // free start the work; this one would get to it only after its
    // handler, however long that runs.
    if (t == nullptr || t->key != this || !t->private_ok ||
        stopped_.load(std::memory_order_relaxed) != 0 ||
        loop_threads_.load(std::memory_order_relaxed) != 1)
        return nullptr;
    return t;
}

inline bool
win_scheduler::try_consume_inline_budget() const noexcept
{
    auto* t = iocp::context_stack.get();
    if (t == nullptr || t->key != this || t->inline_budget <= 0)
        return false;
    --t->inline_budget;
    // Work done without a look at the port, the same as a handler run
    // from the thread's own queue.
    ++t->since_poll;
    return true;
}

inline void
win_scheduler::request_port_visit() const noexcept
{
    auto* t = iocp::context_stack.get();
    if (t != nullptr && t->key == this)
        t->since_poll = iocp::poll_every;
}

inline void
win_scheduler::publish_work(iocp::scheduler_context& t) const noexcept
{
    long const delta = std::exchange(t.work_delta, 0);
    if (delta == 0)
        return;
    if (outstanding_work_.fetch_add(delta, std::memory_order_acq_rel) + delta !=
        0)
        return;
    // Only a loop thread publishes, and its run call retries a failed
    // wakeup on the way out.
    if (auto err = post_stop())
        stop_failure_.store(static_cast<long>(err), std::memory_order_release);
}

inline void
win_scheduler::flush_context(iocp::scheduler_context& t) const noexcept
{
    if (!t.ready.empty() || !t.after_stop.empty())
    {
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            completed_ops_.splice(t.ready);
            completed_ops_.splice(t.after_stop);
            dispatch_required_.fetch_or(
                iocp::dispatch_queue, std::memory_order_seq_cst);
        }
        share_queue_wake();
    }
    publish_work(t);
}

inline void
win_scheduler::share_queue_wake() const noexcept
{
    // A thread blocked in the port learns of the queue only from a
    // packet. A thread about to block counts itself and then reads
    // the queue's bit, and the caller set the bit and now reads the
    // count, each pair in a single total order: either that thread
    // sees the bit and does not block, or it is counted here and
    // woken. One packet in the port is enough: the thread it wakes
    // asks for the next when it leaves more behind. A stopped
    // scheduler wakes its threads to leave, and the next run call
    // looks at the queue first.
    if (blocked_.load(std::memory_order_seq_cst) == 0 ||
        stopped_.load(std::memory_order_acquire) != 0 ||
        wake_pending_.exchange(1, std::memory_order_acq_rel) != 0)
        return;
    if (!::PostQueuedCompletionStatus(iocp_, 0, key_wake_dispatch, nullptr))
        wake_pending_.store(0, std::memory_order_release);
}

inline void
win_scheduler::destroy_entry(void* e) noexcept
{
    if (ready_is_continuation(e))
    {
        auto* c = ready_as_cont(e);
        if (c->h)
            capy::abandon(*c);
        return;
    }
    ready_as_op(e)->destroy();
}

inline bool
win_scheduler::running_in_this_thread() const noexcept
{
    for (auto* c = iocp::context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

inline void
win_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_acq_rel);
}

inline void
win_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    auto err = post_stop();
    if (!err)
        return;
    // This path cannot throw. A run call on this thread retries the
    // wakeup on its way out; on any other thread nothing would ever
    // wake the threads blocked without a timeout.
    if (!running_in_this_thread())
        std::terminate();
    stop_failure_.store(static_cast<long>(err), std::memory_order_release);
}

inline void
win_scheduler::on_pending(overlapped_op* op) const
{
    // If the CAS fails (ready_ was already 1), the completer got here first
    // and stored the results — queue the op, on this thread's own queue or
    // by a re-post, so the loop can dispatch it. The acquire
    // on failure makes those payload writes visible, so the re-posted op
    // carries valid dwError / bytes_transferred.
    //
    // pending_io_ counts the packet that will dispatch this op: on CAS
    // success the kernel's own completion will find ready_ == 1 and
    // dispatch; on CAS failure the kernel's packet was consumed as a
    // skip (uncounted) and the re-post is the dispatching packet. A
    // failed re-post falls back to the deferred queue, which holds the
    // op memory itself — no packet, no count.
    //
    // Counted before the packet can be consumed: once it can, a context
    // whose thread runs it may finish and be destroyed, so this call
    // touches nothing of the scheduler afterwards.
    pending_io_.fetch_add(1, std::memory_order_acq_rel);
    long expected = 0;
    if (op->ready_.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    // The packet is gone and the op is this thread's alone, so the
    // scheduler is still safe to touch here.
    if (auto* t = local_context())
    {
        pending_io_.fetch_sub(1, std::memory_order_acq_rel);
        keep_counted(*t, op);
        return;
    }
    if (::PostQueuedCompletionStatus(
            iocp_, 0, key_result_stored, static_cast<LPOVERLAPPED>(op)))
        return;
    std::lock_guard<win_mutex> lock(dispatch_mutex_);
    completed_ops_.push(op);
    dispatch_required_.fetch_or(
        iocp::dispatch_queue, std::memory_order_seq_cst);
    // Uncounted only once queued, so a shutdown that sees no packet
    // left finds the op in the queue.
    pending_io_.fetch_sub(1, std::memory_order_acq_rel);
}

inline void
win_scheduler::on_completion(overlapped_op* op, DWORD error, DWORD bytes) const
{
    // Synchronous-completion path. Write the payload before the release store
    // to ready_ so the GQCS thread that dequeues the key_result_stored post
    // sees both fields once it observes ready_ == 1.
    op->dwError           = error;
    op->bytes_transferred = bytes;
    op->ready_.store(1, std::memory_order_release);

    // On a loop thread the result needs no packet to reach the loop.
    // The caller counted the work already.
    if (auto* t = local_context())
    {
        keep_counted(*t, op);
        return;
    }

    // Counted before the packet can be consumed, as in on_pending.
    pending_io_.fetch_add(1, std::memory_order_acq_rel);
    if (::PostQueuedCompletionStatus(
            iocp_, 0, key_result_stored, static_cast<LPOVERLAPPED>(op)))
        return;
    std::lock_guard<win_mutex> lock(dispatch_mutex_);
    completed_ops_.push(op);
    dispatch_required_.fetch_or(
        iocp::dispatch_queue, std::memory_order_seq_cst);
    // Uncounted only once queued, so a shutdown that sees no packet
    // left finds the op in the queue.
    pending_io_.fetch_sub(1, std::memory_order_acq_rel);
}

inline unsigned long
win_scheduler::post_stop() const noexcept
{
    stopped_.store(1, std::memory_order_seq_cst);
    return post_wakeup();
}

inline unsigned long
win_scheduler::post_wakeup() const noexcept
{
    if (stop_event_posted_.exchange(1, std::memory_order_acq_rel) != 0)
        return 0;
    if (::PostQueuedCompletionStatus(iocp_, 0, key_shutdown, nullptr))
        return 0;
    // A zero last error would read as a wakeup that was queued.
    auto err = ::GetLastError();
    if (err == 0)
        err = ERROR_INVALID_PARAMETER;
    // No packet is in flight, so a later stop retries the wakeup
    // rather than leaving blocked threads asleep for good.
    stop_event_posted_.store(0, std::memory_order_release);
    return err;
}

inline void
win_scheduler::stop()
{
    // The shutdown post is the only thing that wakes a run()/run_one()
    // thread blocked indefinitely in GQCS. With no periodic timeout
    // there is no fallback, so a failed post is reported rather than
    // ignored. It can only fail under resource exhaustion.
    if (auto err = post_stop())
        throw_wakeup_failure(err);
}

inline void
win_scheduler::throw_wakeup_failure(unsigned long err)
{
    // Kept recorded, so a later run or poll call on the stopped context
    // retries the wakeup instead of blocking while work remains.
    stop_failure_.store(static_cast<long>(err), std::memory_order_release);
    detail::throw_system_error(make_err(err));
}

inline void
win_scheduler::report_stop_failure()
{
    // Nearly always clear, so it is read before it is written.
    if (stop_failure_.load(std::memory_order_acquire) == 0)
        return;
    if (stop_failure_.exchange(0, std::memory_order_acq_rel) == 0 || !stopped())
        return;
    // Other threads may still be blocked waiting for the lost wakeup,
    // so retry it and report only a retry that fails too.
    if (auto err = post_wakeup())
        throw_wakeup_failure(err);
}

inline bool
win_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire) != 0;
}

inline void
win_scheduler::restart()
{
    stopped_.store(0, std::memory_order_release);
    stop_event_posted_.store(0, std::memory_order_release);
    stop_failure_.store(0, std::memory_order_release);
}

inline std::size_t
win_scheduler::run()
{
    report_stop_failure();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    std::size_t n = 0;
    {
        iocp::thread_context_guard ctx(this, true);
        auto& t = ctx.frame_;

        for (;;)
        {
            if (!do_one(t, INFINITE))
                break;
            if (n != (std::numeric_limits<std::size_t>::max)())
                ++n;
            // Queued work is outstanding work, whatever the shared
            // count reads before this thread publishes its part.
            if (!t.ready.empty())
                continue;
            publish_work(t);
            if (outstanding_work_.load(std::memory_order_acquire) == 0)
            {
                stop();
                break;
            }
        }
    }
    report_stop_failure();
    return n;
}

inline std::size_t
win_scheduler::run_one()
{
    report_stop_failure();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    std::size_t n;
    {
        iocp::thread_context_guard ctx(this, false);
        n = do_one(ctx.frame_, INFINITE);
    }
    report_stop_failure();
    return n;
}

inline std::size_t
win_scheduler::wait_one(long usec)
{
    report_stop_failure();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    unsigned long timeout_ms = INFINITE;
    if (usec >= 0)
    {
        auto ms    = (static_cast<long long>(usec) + 999) / 1000;
        timeout_ms = ms >= 0xFFFFFFFELL ? static_cast<unsigned long>(0xFFFFFFFE)
                                        : static_cast<unsigned long>(ms);
    }
    std::size_t n;
    {
        iocp::thread_context_guard ctx(this, false);
        n = do_one(ctx.frame_, timeout_ms);
    }
    report_stop_failure();
    return n;
}

inline std::size_t
win_scheduler::poll()
{
    report_stop_failure();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    std::size_t n = 0;
    {
        iocp::thread_context_guard ctx(this, true);
        while (do_one(ctx.frame_, 0))
            if (n != (std::numeric_limits<std::size_t>::max)())
                ++n;
    }
    report_stop_failure();
    return n;
}

inline std::size_t
win_scheduler::poll_one()
{
    report_stop_failure();
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    std::size_t n;
    {
        iocp::thread_context_guard ctx(this, false);
        n = do_one(ctx.frame_, 0);
    }
    report_stop_failure();
    return n;
}

inline std::size_t
win_scheduler::do_one(iocp::scheduler_context& t, unsigned long timeout_ms)
{
    bool took_wake = false;
    std::size_t n  = do_one_wait(t, timeout_ms, took_wake);
    // Without the timer thread, the wake is all that tells a thread
    // blocked with no timeout about a new earliest timer; leaving
    // with it would strand those threads.
    if (took_wake && !timers_)
    {
        auto* tsvc = timers();
        if (tsvc &&
            tsvc->nearest_expiry() != (timer_service::time_point::max)())
            ::PostQueuedCompletionStatus(iocp_, 0, key_wake_dispatch, nullptr);
    }
    return n;
}

inline void
win_scheduler::claim(
    iocp::scheduler_context& t,
    ready_queue& into,
    void const* entries_v,
    unsigned long count,
    bool& took_wake)
{
    auto const* entries = static_cast<OVERLAPPED_ENTRY const*>(entries_v);
    for (unsigned long i = 0; i < count; ++i)
    {
        auto const& e           = entries[i];
        LPOVERLAPPED overlapped = e.lpOverlapped;
        // What a stop packet leaves behind it in the batch was queued
        // after the stop, and waits for the next run.
        auto& q = t.stop_seen ? t.after_stop : into;

        if (overlapped)
        {
            switch (e.lpCompletionKey)
            {
            case key_io:
            case key_result_stored:
            {
                auto* ov_op = overlapped_to_op(overlapped);

                // key_io carries fresh kernel results — publish them before
                // the CAS so that losing the race (old value 0) still leaves
                // valid data for the on_pending() re-post. For key_result_stored
                // the payload was already written and released (by the completer
                // in on_pending, or by on_completion), so we must not overwrite
                // it.
                if (e.lpCompletionKey == key_io)
                    ov_op->store_result(
                        e.dwNumberOfBytesTransferred,
                        overlapped_error(*overlapped));

                // If old value was 1 the initiator already returned — dispatch.
                // The acquire pairs with the publisher's release so the payload
                // reads in complete() are ordered after the store that produced
                // them. If old value was 0 the initiator hasn't returned yet;
                // skip and let on_pending() re-post.
                long expected = 0;
                if (!ov_op->ready_.compare_exchange_strong(
                        expected, 1, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    pending_io_.fetch_sub(1, std::memory_order_acq_rel);
                    q.push(ov_op);
                }
                break;
            }

            case key_posted:
                // Posted scheduler_op*: overlapped is actually a scheduler_op*
                q.push(reinterpret_cast<scheduler_op*>(overlapped));
                break;

            case key_continuation:
                // Posted continuation: overlapped is actually a continuation*
                q.push(*reinterpret_cast<capy::continuation*>(overlapped));
                break;

            default:   // LCOV_EXCL_LINE unreachable: closed key set
                break; // LCOV_EXCL_LINE unreachable: closed key set
            }
            continue;
        }

        // Signal completions (no OVERLAPPED)
        switch (e.lpCompletionKey)
        {
        case key_wake_dispatch:
            // Wakeup for the timers or the shared queue - loop to
            // check dispatch_required_. Whoever takes the packet goes
            // by the queue before it runs a handler, and asks for
            // another if it leaves work there.
            wake_pending_.store(0, std::memory_order_release);
            took_wake = true;
            break;

        case key_shutdown:
            stop_event_posted_.store(0, std::memory_order_release);
            if (stopped())
            {
                // Re-post for other waiting threads
                if (auto err = post_wakeup())
                    stop_failure_.store(
                        static_cast<long>(err), std::memory_order_release);
                t.stop_seen = true;
            }
            break;

        // A key outside the closed set reaches here only if a
        // third party posts to the port.
        default:   // LCOV_EXCL_LINE unreachable: closed key set
            break; // LCOV_EXCL_LINE unreachable: closed key set
        }
    }
}

inline std::size_t
win_scheduler::do_one_wait(
    iocp::scheduler_context& t, unsigned long timeout_ms, bool& took_wake)
{
    for (;;)
    {
        // The one entry this pass takes from the shared queue, and
        // whether the queue was left alone because the port is due a
        // look first.
        void* shared  = nullptr;
        bool poll_due = false;

        // Check if we need to process timers or the shared queue.
        // Each bit is cleared on its own, so an exception from the
        // timers leaves the queue's bit standing. A thread that has
        // seen the stop leaves the timers alone and only finishes
        // the queue, which was filled ahead of the stop.
        if (long why = dispatch_required_.load(std::memory_order_seq_cst))
        {
            if (!t.stop_seen && (why & iocp::dispatch_timers))
            {
                dispatch_required_.fetch_and(
                    ~iocp::dispatch_timers, std::memory_order_acq_rel);

                if (auto* tsvc = timers())
                {
                    tsvc->process_expired();
                    // Posts made here have no running handler behind
                    // them to keep the shared count above zero.
                    publish_work(t);
                }

                update_timeout();

                // The expired timers' handlers may be on the shared
                // queue now.
                why = dispatch_required_.load(std::memory_order_seq_cst);
            }

            // The thread-private queue of a single-thread context
            // takes the shared one whole, which a stop leaves for the
            // next run.
            if ((why & iocp::dispatch_queue) && !(one_thread_ && t.stop_seen))
            {
                if (!one_thread_ && !t.stop_seen &&
                    ++t.since_poll >= iocp::poll_every)
                {
                    // A chain of posts must not keep the thread from
                    // the port: the queue waits for the visit below.
                    poll_due = true;
                }
                else
                {
                    bool more = false;
                    {
                        std::lock_guard<win_mutex> lock(dispatch_mutex_);
                        if (one_thread_)
                        {
                            t.ready.splice(completed_ops_);
                        }
                        else
                        {
                            // The rest stays where another loop can
                            // take it while this one runs its handler.
                            shared = completed_ops_.pop();
                            more   = !completed_ops_.empty();
                        }
                        // The bit stands while the queue holds work.
                        // It is set and cleared under the lock only.
                        if (!more)
                            dispatch_required_.fetch_and(
                                ~iocp::dispatch_queue,
                                std::memory_order_seq_cst);
                    }
                    if (more)
                        share_queue_wake();
                }
            }
        }

        // A stop ends the run once the work queued ahead of it is
        // done. The frame hands the rest back on the way out. A
        // handler among that work may have restarted the scheduler,
        // and then the run carries on with what was held back.
        if (t.stop_seen && shared == nullptr && t.ready.empty())
        {
            if (stopped())
                return 0;
            t.stop_seen = false;
            if (one_thread_)
            {
                t.ready.splice(t.after_stop);
            }
            else if (!t.after_stop.empty())
            {
                // Held back by this thread, but any loop may run it.
                {
                    std::lock_guard<win_mutex> lock(dispatch_mutex_);
                    completed_ops_.splice(t.after_stop);
                    dispatch_required_.fetch_or(
                        iocp::dispatch_queue, std::memory_order_seq_cst);
                }
                share_queue_wake();
                continue;
            }
        }

        // With work in hand the thread only looks at the port now and
        // then, without waiting; with an entry of the shared queue in
        // hand, not at all, so nothing can come between taking the
        // entry and running it.
        bool const visit = shared == nullptr &&
            (poll_due || t.ready.empty() ||
             (!t.stop_seen && ++t.since_poll >= iocp::poll_every));
        if (visit)
        {
            t.since_poll = 0;

            auto* tsvc = timers();
            if (tsvc && !timers_)
            {
                tsvc->process_expired();
                // As above: these posts are made outside any handler.
                publish_work(t);
            }

            // The thread counts itself as blocked before its last
            // look at the shared queue, so work queued from now on
            // comes with a wakeup (see share_queue_wake). The queue
            // may hold work this pass did not take: the port was due
            // a look first, the timers above just queued it, or
            // another thread did.
            bool idle = t.ready.empty() && !poll_due;
            if (idle)
            {
                blocked_.fetch_add(1, std::memory_order_seq_cst);
                if (dispatch_required_.load(std::memory_order_seq_cst) &
                    iocp::dispatch_queue)
                {
                    blocked_.fetch_sub(1, std::memory_order_acq_rel);
                    idle = false;
                }
            }
            bool const counted_blocked = idle;
            unsigned long wait_ms      = idle ? timeout_ms : 0;
            if (idle && tsvc && !timers_)
                wait_ms = timer_timeout_ms(*tsvc, timeout_ms);

            // Nothing queued, so the shared count has to be exact
            // before this thread can block on it.
            if (idle)
                publish_work(t);

            // With another loop running, the port itself hands each
            // packet to whichever thread is free, so a thread takes a
            // batch only while it is the one loop there is.
            ULONG const cap =
                (!t.private_ok ||
                 loop_threads_.load(std::memory_order_relaxed) != 1)
                ? 1
                : iocp::max_batch;

            OVERLAPPED_ENTRY entries[iocp::max_batch];
            ULONG count = 0;

            BOOL const result = ::GetQueuedCompletionStatusEx(
                iocp_, entries, cap, &count, wait_ms, FALSE);
            DWORD const dwError = result ? 0 : ::GetLastError();
            if (counted_blocked)
                blocked_.fetch_sub(1, std::memory_order_acq_rel);

            if (result)
            {
                ready_queue batch;
                claim(t, batch, entries, count, took_wake);
                // A second loop may start while the first handler of
                // a batch runs, and has to be able to run the rest:
                // the batch goes where it can be reached, the part
                // ahead of a stop included.
                if (one_thread_ || count == 1 || batch.empty())
                {
                    t.ready.splice(batch);
                }
                else
                {
                    {
                        std::lock_guard<win_mutex> lock(dispatch_mutex_);
                        completed_ops_.splice(batch);
                        dispatch_required_.fetch_or(
                            iocp::dispatch_queue, std::memory_order_seq_cst);
                    }
                    share_queue_wake();
                    continue;
                }
            }
            else
            {
                // Timeout or error. Unless a timer cut the wait short, a
                // WAIT_TIMEOUT is a finite caller timeout (wait_one/poll)
                // elapsing with no work. run()/run_one() pass INFINITE and
                // block until a real wake-up; they exit only once a
                // key_shutdown is posted.
                if (dwError != WAIT_TIMEOUT)
                    detail::throw_system_error(make_err(dwError));
                if (idle)
                {
                    if (wait_ms == timeout_ms)
                        return 0;
                    if (timeout_ms != INFINITE)
                        timeout_ms -= wait_ms;
                    continue;
                }
            }
        }

        // The shared queue's entry was queued before anything this
        // thread holds.
        void* const e = shared != nullptr ? shared : t.ready.pop();
        if (!e)
            continue;

        // Work queued behind this handler would wait for it while
        // another loop could run it: one that started since the work
        // was queued, or the others when this thread drained the
        // shared queue. Hand it over.
        if (!t.stop_seen && !t.ready.empty() &&
            loop_threads_.load(std::memory_order_relaxed) != 1)
            flush_context(t);

        t.inline_budget = inline_budget_max_;

        // Counted when the handler leaves, by return or by exception.
        struct finish
        {
            iocp::scheduler_context& t;
            ~finish()
            {
                --t.work_delta;
            }
        } on_exit{t};

        if (ready_is_continuation(e))
            ready_as_cont(e)->h.resume();
        else
            ready_as_op(e)->complete(this, 0, 0);
        return 1;
    }
}

inline void
win_scheduler::on_timer_changed(void* ctx)
{
    static_cast<win_scheduler*>(ctx)->update_timeout();
}

// Rounds up, so a timer is never seen early and polled for again.
inline unsigned long
win_scheduler::timer_timeout_ms(
    timer_service& svc, unsigned long timeout_ms) noexcept
{
    auto nearest = svc.nearest_expiry();
    if (nearest == (timer_service::time_point::max)())
        return timeout_ms;
    auto now = std::chrono::steady_clock::now();
    if (nearest <= now)
        return 0;
    auto ms =
        std::chrono::ceil<std::chrono::milliseconds>(nearest - now).count();
    // Kept below INFINITE, which would never time out.
    ms = (std::min)(ms, static_cast<decltype(ms)>(INFINITE - 1));
    return (std::min)(timeout_ms, static_cast<unsigned long>(ms));
}

inline void
win_scheduler::on_timer_service_attached(timer_service& svc)
{
    // Without the timer thread the completion-port wait timeout drives
    // timers instead. The first timer is often made inside a delay's
    // await_suspend, where a throw could only terminate.
    auto timers = make_win_timers(iocp_, &dispatch_required_);
    if (timers->start())
        timers_ = std::move(timers);
    svc.set_on_earliest_changed(
        timer_service::callback{this, &on_timer_changed});
}

inline void
win_scheduler::update_timeout()
{
    auto* tsvc = timers();
    if (!tsvc)
        return;
    if (timers_)
    {
        // Read and arm under one lock. Two threads arming at once could
        // otherwise read different earliest expiries and set the timer
        // in the opposite order, leaving the later deadline set and the
        // earlier timer firing late. A new earliest expiry is published
        // before its notification takes this lock, so the last thread
        // through arms the earliest one.
        std::lock_guard<win_mutex> lock(dispatch_mutex_);
        timers_->update_timeout(tsvc->nearest_expiry());
    }
    else
    {
        // A run loop may be waiting past the new earliest expiry.
        ::PostQueuedCompletionStatus(iocp_, 0, key_wake_dispatch, nullptr);
    }
}

} // namespace boost::corosio::detail

// Defer including the auxiliary wait reactor until the scheduler is
// fully defined, since the reactor's inline methods call back into
// win_scheduler. This also gives the ctor, dtor and wait_reactor()
// below a complete win_wait_reactor type: the constructor builds it,
// the destructor and unique_ptr's deleter tear it down.
//
// The macro lets win_wait_reactor.hpp diagnose direct inclusion
// (which would land it here with win_scheduler still incomplete).
#define BOOST_COROSIO_DETAIL_IOCP_WIN_SCHEDULER_BODY_DONE
#include <boost/corosio/native/detail/iocp/win_wait_reactor.hpp>

namespace boost::corosio::detail {

inline win_scheduler::win_scheduler(
    capy::execution_context&, int concurrency_hint)
    : iocp_(nullptr)
    , outstanding_work_(0)
    , stopped_(0)
    , stop_event_posted_(0)
    , dispatch_required_(0)
{
    // concurrency_hint < 0 means use system default (DWORD(~0) = max)
    iocp_ = ::CreateIoCompletionPort(
        INVALID_HANDLE_VALUE, nullptr, 0,
        static_cast<DWORD>(
            concurrency_hint >= 0 ? concurrency_hint : DWORD(~0)));

    if (iocp_ == nullptr)
        detail::throw_system_error(make_err(::GetLastError()));

    try
    {
        // A scheduler whose wait reactor could not be built would
        // answer every wait with a parked op, so it refuses to exist
        // instead.
        wait_reactor_ = std::make_unique<win_wait_reactor>(*this);
    }
    catch (...)
    {
        // ~win_scheduler never runs for a constructor that throws, and
        // the port is a raw handle nothing else owns.
        ::CloseHandle(iocp_);
        iocp_ = nullptr;
        throw;
    }
}

inline void
win_scheduler::shutdown()
{
    if (timers_)
        timers_->stop();

    // Ops parked in the auxiliary wait reactor owe completion packets.
    // Stop the reactor early so its loop posts them as cancelled and
    // the pending count can reach zero.
    wait_reactor_->stop();

    wait_cancels_posting();

    // Reap every packet still owed to the port before the services
    // free the op memory those packets reference. Work-guard credits,
    // posted handlers, and queued continuations have no bearing here.
    while (pending_io_.load(std::memory_order_acquire) > 0)
    {
        ready_queue ops;
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            ops.splice(completed_ops_);
        }

        // Deferred-queue entries are process-owned (failed-post
        // fallbacks, posted handlers, and what a run loop handed
        // back); no packet references them.
        while (auto* e = ops.pop())
            destroy_entry(e);

        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;
        ::GetQueuedCompletionStatus(
            iocp_, &bytes, &key, &overlapped, iocp::shutdown_drain_timeout_ms);
        if (overlapped)
        {
            if (key == key_posted)
            {
                auto* op = reinterpret_cast<scheduler_op*>(overlapped);
                op->destroy();
            }
            else if (key == key_continuation)
            {
                // Drain without resuming: abandon the parked coroutine.
                auto* c = reinterpret_cast<capy::continuation*>(overlapped);
                if (c->h)
                    capy::abandon(*c);
            }
            else
            {
                pending_io_.fetch_sub(1, std::memory_order_acq_rel);
                auto* op = overlapped_to_op(overlapped);
                op->destroy();
            }
        }
    }

    // Final sweep: packets can sit in the port or deferred queue
    // without a pending_io_ count (posted directly against the
    // handle). Destroy them so service teardown does not free state
    // they still reference.
    sweep_port();
}

inline void
win_scheduler::sweep_port() noexcept
{
    for (;;)
    {
        ready_queue ops;
        {
            std::lock_guard<win_mutex> lock(dispatch_mutex_);
            ops.splice(completed_ops_);
        }
        while (auto* e = ops.pop())
            destroy_entry(e);

        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;
        BOOL const dequeued =
            ::GetQueuedCompletionStatus(iocp_, &bytes, &key, &overlapped, 0);
        if (!overlapped)
        {
            // A signal such as stop()'s carries no operation, and
            // packets posted after it still need sweeping; only an
            // empty port ends the sweep.
            if (!dequeued)
                break;
            continue;
        }
        if (key == key_posted)
        {
            reinterpret_cast<scheduler_op*>(overlapped)->destroy();
        }
        else if (key == key_continuation)
        {
            auto* c = reinterpret_cast<capy::continuation*>(overlapped);
            if (c->h)
                capy::abandon(*c);
        }
        else
        {
            overlapped_to_op(overlapped)->destroy();
        }
    }
}

inline win_scheduler::~win_scheduler()
{
    // A timer made after shutdown starts a thread that posts to the
    // port, so it is joined before the port closes.
    timers_.reset();
    wait_reactor_->stop();
    wait_reactor_.reset();

    if (iocp_ != nullptr)
    {
        // Services give up their work in shutdown(), but anything posted
        // after the drain, such as from a destructor, is still destroyed.
        sweep_port();
        ::CloseHandle(iocp_);
    }
}

inline win_wait_reactor&
win_scheduler::wait_reactor()
{
    return *wait_reactor_;
}

inline void
win_scheduler::cancel_wait(overlapped_op* op) noexcept
{
    wait_reactor_->cancel_wait(op);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SCHEDULER_HPP
