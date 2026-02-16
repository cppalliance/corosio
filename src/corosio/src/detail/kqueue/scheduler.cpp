//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/scheduler.hpp"
#include "src/detail/kqueue/op.hpp"
#include "src/detail/timer_service.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <limits>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
    kqueue Scheduler - Single Reactor Model
    ========================================

    This scheduler uses the same thread coordination strategy as the epoll
    backend to provide handler parallelism and avoid the thundering herd problem.
    Instead of all threads blocking on kevent(), one thread becomes the
    "reactor" while others wait on a condition variable for handler work.

    Thread Model
    ------------
    - ONE thread runs kevent() at a time (the reactor thread)
    - OTHER threads wait on cond_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()
    - This matches Windows IOCP semantics where N posted items wake N threads

    Event Loop Structure (do_one)
    -----------------------------
    1. Lock mutex, try to pop handler from queue
    2. If got handler: execute it (unlocked), return
    3. If queue empty and no reactor running: become reactor
       - Run kevent() (unlocked), queue I/O completions, loop back
    4. If queue empty and reactor running: wait on condvar for work

    kqueue-Specific Design
    ----------------------
    - Uses EVFILT_USER for reactor interruption (no extra fd needed)
    - Uses EV_CLEAR for edge-triggered semantics (equivalent to EPOLLET)
    - Timer expiry computed from timer_service, passed as kevent() timeout
    - No timerfd equivalent; uses software timer queue

    Signaling State (state_)
    ------------------------
    Same as epoll: bit 0 = signaled, upper bits = waiter count.
*/

namespace boost::corosio::detail {

struct scheduler_context
{
    kqueue_scheduler const* key;
    scheduler_context* next;
    op_queue private_queue;
    std::int64_t private_outstanding_work;
    int inline_budget;

    scheduler_context(kqueue_scheduler const* k, scheduler_context* n)
        : key(k)
        , next(n)
        , private_outstanding_work(0)
        , inline_budget(0)
    {
    }
};

namespace {

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(kqueue_scheduler const* ctx) noexcept
        : frame_(ctx, context_stack.get())
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        if (!frame_.private_queue.empty())
            frame_.key->drain_thread_queue(
                frame_.private_queue, frame_.private_outstanding_work);
        context_stack.set(frame_.next);
    }
};

scheduler_context*
find_context(kqueue_scheduler const* self) noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == self)
            return c;
    return nullptr;
}

/// Flush private work count to global counter.
void
flush_private_work(
    scheduler_context* ctx,
    std::atomic<std::int64_t>& outstanding_work) noexcept
{
    if (ctx && ctx->private_outstanding_work > 0)
    {
        outstanding_work.fetch_add(
            ctx->private_outstanding_work, std::memory_order_relaxed);
        ctx->private_outstanding_work = 0;
    }
}

/// Drain private queue to global queue, flushing work count first.
///
/// @return True if any ops were drained.
bool
drain_private_queue(
    scheduler_context* ctx,
    std::atomic<std::int64_t>& outstanding_work,
    op_queue& completed_ops) noexcept
{
    if (!ctx || ctx->private_queue.empty())
        return false;

    flush_private_work(ctx, outstanding_work);
    completed_ops.splice(ctx->private_queue);
    return true;
}

} // namespace

void
kqueue_scheduler::reset_inline_budget() const noexcept
{
    if (auto* ctx = find_context(this))
        ctx->inline_budget = max_inline_budget_;
}

bool
kqueue_scheduler::try_consume_inline_budget() const noexcept
{
    if (auto* ctx = find_context(this))
    {
        if (ctx->inline_budget > 0)
        {
            --ctx->inline_budget;
            return true;
        }
    }
    return false;
}

void
descriptor_state::operator()()
{
    // Release ensures the false is visible to the reactor's CAS on other
    // cores. With relaxed, ARM's store buffer can delay the write,
    // causing the reactor's CAS to see a stale 'true' and skip
    // enqueue—permanently losing the edge-triggered event and
    // eventually deadlocking. On x86 (TSO) release compiles to the
    // same MOV as relaxed, so there is no cost there.
    is_enqueued_.store(false, std::memory_order_release);

    // Take ownership of impl ref set by close_socket() to prevent
    // the owning impl from being freed while we're executing
    auto prevent_impl_destruction = std::move(impl_ref_);

    std::uint32_t ev = ready_events_.exchange(0, std::memory_order_acquire);
    if (ev == 0)
    {
        scheduler_->compensating_work_started();
        return;
    }

    op_queue local_ops;

    int err = 0;
    if (ev & kqueue_event_error)
    {
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        if (err == 0)
            err = EIO;
    }

    kqueue_op* rd = nullptr;
    kqueue_op* wr = nullptr;
    kqueue_op* cn = nullptr;
    {
        std::lock_guard lock(mutex);
        if (ev & kqueue_event_read)
        {
            rd = std::exchange(read_op, nullptr);
            if (!rd)
                read_ready = true;
        }
        if (ev & kqueue_event_write)
        {
            cn = std::exchange(connect_op, nullptr);
            wr = std::exchange(write_op, nullptr);
            if (!cn && !wr)
                write_ready = true;
        }
        if (err && !(ev & (kqueue_event_read | kqueue_event_write)))
        {
            rd = std::exchange(read_op, nullptr);
            wr = std::exchange(write_op, nullptr);
            cn = std::exchange(connect_op, nullptr);
        }
    }

    // Non-null after I/O means EAGAIN; re-register under lock below
    if (rd)
    {
        if (err)
            rd->complete(err, 0);
        else
            rd->perform_io();

        if (rd->errn == EAGAIN || rd->errn == EWOULDBLOCK)
        {
            rd->errn = 0;
        }
        else
        {
            local_ops.push(rd);
            rd = nullptr;
        }
    }

    if (cn)
    {
        if (err)
            cn->complete(err, 0);
        else
            cn->perform_io();
        local_ops.push(cn);
        cn = nullptr;
    }

    if (wr)
    {
        if (err)
            wr->complete(err, 0);
        else
            wr->perform_io();

        if (wr->errn == EAGAIN || wr->errn == EWOULDBLOCK)
        {
            wr->errn = 0;
        }
        else
        {
            local_ops.push(wr);
            wr = nullptr;
        }
    }

    // Re-register EAGAIN ops. A concurrent operator()() invocation may
    // have set read_ready/write_ready while we held the op (no read_op
    // was registered, so it cached the edge event). Check the flags
    // under the same lock as re-registration so no edge is lost.
    while (rd || wr)
    {
        bool retry = false;
        {
            std::lock_guard lock(mutex);
            if (rd)
            {
                if (read_ready)
                {
                    read_ready = false;
                    retry = true;
                }
                else
                {
                    read_op = rd;
                    rd = nullptr;
                }
            }
            if (wr)
            {
                if (write_ready)
                {
                    write_ready = false;
                    retry = true;
                }
                else
                {
                    write_op = wr;
                    wr = nullptr;
                }
            }
        }

        if (!retry)
            break;

        if (rd)
        {
            rd->perform_io();
            if (rd->errn == EAGAIN || rd->errn == EWOULDBLOCK)
                rd->errn = 0;
            else
            {
                local_ops.push(rd);
                rd = nullptr;
            }
        }
        if (wr)
        {
            wr->perform_io();
            if (wr->errn == EAGAIN || wr->errn == EWOULDBLOCK)
                wr->errn = 0;
            else
            {
                local_ops.push(wr);
                wr = nullptr;
            }
        }
    }

    // Execute first handler inline — the scheduler's work_cleanup
    // accounts for this as the "consumed" work item
    scheduler_op* first = local_ops.pop();
    if (first)
    {
        scheduler_->post_deferred_completions(local_ops);
        (*first)();
    }
    else
    {
        scheduler_->compensating_work_started();
    }
}

kqueue_scheduler::kqueue_scheduler(capy::execution_context& ctx, int)
    : kq_fd_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
    , task_running_(false)
    , task_interrupted_(false)
    , state_(0)
{
    // FreeBSD 13+: kqueue1(O_CLOEXEC) available
    kq_fd_ = ::kqueue();
    if (kq_fd_ < 0)
        detail::throw_system_error(make_err(errno), "kqueue");

    if (::fcntl(kq_fd_, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(kq_fd_);
        detail::throw_system_error(make_err(errn), "fcntl (kqueue FD_CLOEXEC)");
    }

    // Register EVFILT_USER for reactor interruption (no self-pipe fallback).
    // Requires FreeBSD 11+ or macOS 10.6+; fails with throw on older kernels.
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
    {
        int errn = errno;
        ::close(kq_fd_);
        detail::throw_system_error(make_err(errn), "kevent (EVFILT_USER)");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(this, [](void* p) {
            static_cast<kqueue_scheduler*>(p)->interrupt_reactor();
        }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);

    // Push task sentinel to interleave reactor runs with handler execution
    completed_ops_.push(&task_op_);
}

kqueue_scheduler::~kqueue_scheduler()
{
    if (kq_fd_ >= 0)
        ::close(kq_fd_);
}

void
kqueue_scheduler::shutdown()
{
    {
        std::unique_lock lock(mutex_);
        shutdown_ = true;

        while (auto* h = completed_ops_.pop())
        {
            if (h == &task_op_)
                continue;
            lock.unlock();
            h->destroy();
            lock.lock();
        }

        signal_all(lock);
    }

    outstanding_work_.store(0, std::memory_order_release);

    if (kq_fd_ >= 0)
        interrupt_reactor();
}

void
kqueue_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;

        explicit post_handler(std::coroutine_handle<> h) : h_(h) {}

        ~post_handler() = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
            // Acquire fence on *this thread* (not the deleted object) ensures
            // stores made by the posting thread (e.g. coroutine state written
            // before the cross-thread post) are visible before we resume.
            std::atomic_thread_fence(std::memory_order_acquire);
            h.resume();
        }

        void destroy() override
        {
            delete this;
        }
    };

    auto ph = std::make_unique<post_handler>(h);

    // Fast path: same thread posts to private queue
    // Only count locally; work_cleanup batches to global counter
    if (auto* ctx = find_context(this))
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(ph.release());
        return;
    }

    // Slow path: cross-thread post requires mutex
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(ph.release());
    wake_one_thread_and_unlock(lock);
}

void
kqueue_scheduler::post(scheduler_op* h) const
{
    // Fast path: same thread posts to private queue
    // Only count locally; work_cleanup batches to global counter
    if (auto* ctx = find_context(this))
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(h);
        return;
    }

    // Slow path: cross-thread post requires mutex
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

bool
kqueue_scheduler::running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
kqueue_scheduler::stop()
{
    std::unique_lock lock(mutex_);
    if (!stopped_.load(std::memory_order_relaxed))
    {
        stopped_.store(true, std::memory_order_release);
        signal_all(lock);
        interrupt_reactor();
    }
}

bool
kqueue_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
kqueue_scheduler::restart()
{
    std::unique_lock lock(mutex_);
    stopped_.store(false, std::memory_order_release);
}

std::size_t
kqueue_scheduler::run()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    std::unique_lock lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, -1, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

std::size_t
kqueue_scheduler::run_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    std::unique_lock lock(mutex_);
    return do_one(lock, -1, &ctx.frame_);
}

std::size_t
kqueue_scheduler::wait_one(long usec)
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    std::unique_lock lock(mutex_);
    return do_one(lock, usec, &ctx.frame_);
}

std::size_t
kqueue_scheduler::poll()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    std::unique_lock lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, 0, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

std::size_t
kqueue_scheduler::poll_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    std::unique_lock lock(mutex_);
    return do_one(lock, 0, &ctx.frame_);
}

void
kqueue_scheduler::register_descriptor(int fd, descriptor_state* desc) const
{
    struct kevent changes[2];
    EV_SET(
        &changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_CLEAR,
        0, 0, desc);
    EV_SET(
        &changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE,
        EV_ADD | EV_CLEAR, 0, 0, desc);

    if (::kevent(kq_fd_, changes, 2, nullptr, 0, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "kevent (register)");

    desc->registered_events = kqueue_event_read | kqueue_event_write;
    desc->fd = fd;
    desc->scheduler_ = this;

    std::lock_guard lock(desc->mutex);
    desc->read_ready = false;
    desc->write_ready = false;
}

void
kqueue_scheduler::deregister_descriptor(int fd) const
{
    struct kevent changes[2];
    EV_SET(
        &changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0,
        nullptr);
    EV_SET(
        &changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0,
        nullptr);
    // Ignore errors - fd may already be closed (kqueue auto-removes on close)
    ::kevent(kq_fd_, changes, 2, nullptr, 0, nullptr);
}

void
kqueue_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
kqueue_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

void
kqueue_scheduler::compensating_work_started() const noexcept
{
    auto* ctx = find_context(this);
    if (ctx)
        ++ctx->private_outstanding_work;
}

void
kqueue_scheduler::drain_thread_queue(op_queue& queue, std::int64_t count) const
{
    // Flush private work count to global counter — private posts
    // only incremented the thread-local counter, not outstanding_work_
    if (count > 0)
        outstanding_work_.fetch_add(count, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.splice(queue);
    if (count > 0)
        maybe_unlock_and_signal_one(lock);
}

void
kqueue_scheduler::post_deferred_completions(op_queue& ops) const
{
    if (ops.empty())
        return;

    // Fast path: if on scheduler thread, use private queue
    if (auto* ctx = find_context(this))
    {
        ctx->private_queue.splice(ops);
        return;
    }

    // Slow path: add to global queue and wake a thread
    std::unique_lock lock(mutex_);
    completed_ops_.splice(ops);
    wake_one_thread_and_unlock(lock);
}

void
kqueue_scheduler::interrupt_reactor() const
{
    // Only trigger if not already armed to avoid redundant triggers.
    // acq_rel: release makes the true store visible to the reactor;
    // acquire on failure sees the reactor's release store of false,
    // preventing a stale-true read that would silently drop the trigger.
    // On x86 (TSO) this compiles to the same LOCK CMPXCHG as before.
    bool expected = false;
    if (user_event_armed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        struct kevent ev;
        EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr);
    }
}

void
kqueue_scheduler::signal_all(std::unique_lock<std::mutex>&) const
{
    state_ |= signaled_bit;
    cond_.notify_all();
}

bool
kqueue_scheduler::maybe_unlock_and_signal_one(
    std::unique_lock<std::mutex>& lock) const
{
    state_ |= signaled_bit;
    if (state_ > signaled_bit)
    {
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    return false;
}

void
kqueue_scheduler::unlock_and_signal_one(
    std::unique_lock<std::mutex>& lock) const
{
    state_ |= signaled_bit;
    bool have_waiters = state_ > signaled_bit;
    lock.unlock();
    if (have_waiters)
        cond_.notify_one();
}

void
kqueue_scheduler::clear_signal() const
{
    state_ &= ~signaled_bit;
}

void
kqueue_scheduler::wait_for_signal(std::unique_lock<std::mutex>& lock) const
{
    while ((state_ & signaled_bit) == 0)
    {
        state_ += waiter_increment;
        cond_.wait(lock);
        state_ -= waiter_increment;
    }
}

void
kqueue_scheduler::wait_for_signal_for(
    std::unique_lock<std::mutex>& lock, long timeout_us) const
{
    if ((state_ & signaled_bit) == 0)
    {
        state_ += waiter_increment;
        cond_.wait_for(lock, std::chrono::microseconds(timeout_us));
        state_ -= waiter_increment;
    }
}

void
kqueue_scheduler::wake_one_thread_and_unlock(
    std::unique_lock<std::mutex>& lock) const
{
    if (maybe_unlock_and_signal_one(lock))
        return;

    if (task_running_ && !task_interrupted_)
    {
        task_interrupted_ = true;
        lock.unlock();
        interrupt_reactor();
    }
    else
    {
        lock.unlock();
    }
}

long
kqueue_scheduler::calculate_timeout(long requested_timeout_us) const
{
    if (requested_timeout_us == 0)
        return 0;

    auto nearest = timer_svc_->nearest_expiry();
    if (nearest == timer_service::time_point::max())
        return requested_timeout_us;

    auto now = std::chrono::steady_clock::now();
    if (nearest <= now)
        return 0;

    auto timer_timeout_us =
        std::chrono::duration_cast<std::chrono::microseconds>(nearest - now)
            .count();

    // Clamp to [0, LONG_MAX] to prevent truncation on 32-bit long platforms
    constexpr auto long_max =
        static_cast<long long>((std::numeric_limits<long>::max)());
    auto capped_timer_us = std::min(
        std::max(timer_timeout_us, static_cast<long long>(0)), long_max);

    if (requested_timeout_us < 0)
        return static_cast<long>(capped_timer_us);

    // requested_timeout_us is already long, so min() result fits in long
    return static_cast<long>(std::min(
        static_cast<long long>(requested_timeout_us), capped_timer_us));
}

/** RAII guard for handler execution work accounting.

    Handler consumes 1 work item, may produce N new items via fast-path posts.
    Net change = N - 1:
    - If N > 1: add (N-1) to global (more work produced than consumed)
    - If N == 1: net zero, do nothing
    - If N < 1: call work_finished() (work consumed, may trigger stop)

    Also drains private queue to global for other threads to process.
*/
struct work_cleanup
{
    kqueue_scheduler* scheduler;
    std::unique_lock<std::mutex>* lock;
    scheduler_context* ctx;

    ~work_cleanup()
    {
        if (ctx)
        {
            std::int64_t produced = ctx->private_outstanding_work;
            if (produced > 1)
                scheduler->outstanding_work_.fetch_add(
                    produced - 1, std::memory_order_relaxed);
            else if (produced < 1)
                scheduler->work_finished();
            // produced == 1: net zero, handler consumed what it produced
            ctx->private_outstanding_work = 0;

            if (!ctx->private_queue.empty())
            {
                lock->lock();
                scheduler->completed_ops_.splice(ctx->private_queue);
            }
        }
        else
        {
            // No thread context - slow-path op was already counted globally
            scheduler->work_finished();
        }
    }
};

/** RAII guard for reactor work accounting.

    Reactor only produces work via timer/signal callbacks posting handlers.
    Unlike handler execution which consumes 1, the reactor consumes nothing.
    All produced work must be flushed to global counter.
*/
struct task_cleanup
{
    kqueue_scheduler const* scheduler;
    scheduler_context* ctx;

    ~task_cleanup()
    {
        if (ctx && ctx->private_outstanding_work > 0)
        {
            scheduler->outstanding_work_.fetch_add(
                ctx->private_outstanding_work, std::memory_order_relaxed);
            ctx->private_outstanding_work = 0;
        }
    }
};

void
kqueue_scheduler::run_task(
    std::unique_lock<std::mutex>& lock, scheduler_context* ctx)
{
    long effective_timeout_us = task_interrupted_ ? 0 : calculate_timeout(-1);

    if (lock.owns_lock())
        lock.unlock();

    // Flush private work count when reactor completes
    task_cleanup on_exit{this, ctx};
    (void)on_exit;

    // Convert timeout to timespec for kevent()
    struct timespec ts;
    struct timespec* ts_ptr = nullptr;
    if (effective_timeout_us >= 0)
    {
        ts.tv_sec = effective_timeout_us / 1000000;
        ts.tv_nsec = (effective_timeout_us % 1000000) * 1000;
        ts_ptr = &ts;
    }

    // Event loop runs without mutex held
    struct kevent events[128];
    int nev = ::kevent(kq_fd_, nullptr, 0, events, 128, ts_ptr);
    int saved_errno = errno;

    if (nev < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "kevent");

    op_queue local_ops;
    std::int64_t completions_queued = 0;

    // Process events without holding the mutex
    for (int i = 0; i < nev; ++i)
    {
        if (events[i].filter == EVFILT_USER)
        {
            // Interrupt event - clear the armed flag.
            // Release pairs with the acquire CAS failure path in
            // interrupt_reactor(), ensuring the reactor sees our
            // store of false and can re-arm the EVFILT_USER trigger.
            // On x86 (TSO) this compiles identically to relaxed.
            user_event_armed_.store(false, std::memory_order_release);
            continue;
        }

        auto* desc = static_cast<descriptor_state*>(events[i].udata);
        if (!desc)
            continue;

        // Map kqueue events to ready-event flags
        std::uint32_t ready = 0;

        if (events[i].filter == EVFILT_READ)
            ready |= kqueue_event_read;
        else if (events[i].filter == EVFILT_WRITE)
            ready |= kqueue_event_write;

        if (events[i].flags & EV_ERROR)
            ready |= kqueue_event_error;

        // EV_EOF: peer closed or error condition
        if (events[i].flags & EV_EOF)
        {
            // EV_EOF on a read filter means the peer closed — deliver as
            // a read event so the read returns 0 (EOF)
            if (events[i].filter == EVFILT_READ)
                ready |= kqueue_event_read;
            // fflags contains the socket error (if any) when EV_EOF is set
            if (events[i].fflags != 0)
                ready |= kqueue_event_error;
        }

        desc->add_ready_events(ready);

        // Only enqueue if not already enqueued.
        // acq_rel on success: release makes add_ready_events visible
        // to the consumer's acquire exchange; acquire pairs with the
        // consumer's release store of false so we read the latest
        // value. acquire on failure: ensures the CAS load sees the
        // consumer's release store on ARM (prevents stale reads from
        // the store buffer). On x86 (TSO) these compile identically
        // to the weaker orderings.
        bool expected = false;
        if (desc->is_enqueued_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            local_ops.push(desc);
            ++completions_queued;
        }
    }

    // Process timers after kevent returns
    timer_svc_->process_expired();

    // --- Acquire mutex only for queue operations ---
    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);

    // Drain private queue to global — flush work count BEFORE splicing
    // so consumer threads can't decrement outstanding_work_ to zero
    // before the count reflects the newly visible operations.
    if (ctx && !ctx->private_queue.empty())
    {
        if (ctx->private_outstanding_work > 0)
        {
            outstanding_work_.fetch_add(
                ctx->private_outstanding_work, std::memory_order_relaxed);
            completions_queued += ctx->private_outstanding_work;
            ctx->private_outstanding_work = 0;
        }
        completed_ops_.splice(ctx->private_queue);
    }

    // Signal and wake one waiter if work is queued
    if (completions_queued > 0)
    {
        if (maybe_unlock_and_signal_one(lock))
            lock.lock();
    }
}

std::size_t
kqueue_scheduler::do_one(
    std::unique_lock<std::mutex>& lock, long timeout_us, scheduler_context* ctx)
{
    for (;;)
    {
        if (stopped_.load(std::memory_order_relaxed))
            return 0;

        scheduler_op* op = completed_ops_.pop();

        // Handle reactor sentinel - time to poll for I/O
        if (op == &task_op_)
        {
            bool more_handlers =
                !completed_ops_.empty() || (ctx && !ctx->private_queue.empty());

            // Nothing to run the reactor for: no pending work to wait on,
            // or caller requested a non-blocking poll
            if (!more_handlers &&
                (outstanding_work_.load(std::memory_order_acquire) == 0 ||
                 timeout_us == 0))
            {
                completed_ops_.push(&task_op_);
                return 0;
            }

            task_interrupted_ = more_handlers || timeout_us == 0;
            task_running_ = true;

            if (more_handlers)
                unlock_and_signal_one(lock);

            try
            {
                run_task(lock, ctx);
            }
            catch (...)
            {
                task_running_ = false;
                throw;
            }

            task_running_ = false;
            completed_ops_.push(&task_op_);
            continue;
        }

        // Handle operation
        if (op != nullptr)
        {
            if (!completed_ops_.empty())
                unlock_and_signal_one(lock);
            else
                lock.unlock();

            work_cleanup on_exit{this, &lock, ctx};
            (void)on_exit;

            (*op)();
            return 1;
        }

        // No work from global queue - try private queue before blocking
        if (drain_private_queue(ctx, outstanding_work_, completed_ops_))
            continue;

        // No pending work to wait on, or caller requested non-blocking poll
        if (outstanding_work_.load(std::memory_order_acquire) == 0 ||
            timeout_us == 0)
            return 0;

        clear_signal();
        if (timeout_us < 0)
            wait_for_signal(lock);
        else
            wait_for_signal_for(lock, timeout_us);
    }
}

} // namespace boost::corosio::detail

#endif
