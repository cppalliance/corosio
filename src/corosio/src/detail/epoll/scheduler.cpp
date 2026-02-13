//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include "src/detail/epoll/scheduler.hpp"
#include "src/detail/epoll/op.hpp"
#include "src/detail/timer_service.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <chrono>
#include <limits>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

/*
    epoll Scheduler - Single Reactor Model
    ======================================

    This scheduler uses a thread coordination strategy to provide handler
    parallelism and avoid the thundering herd problem.
    Instead of all threads blocking on epoll_wait(), one thread becomes the
    "reactor" while others wait on a condition variable for handler work.

    Thread Model
    ------------
    - ONE thread runs epoll_wait() at a time (the reactor thread)
    - OTHER threads wait on cond_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()
    - This matches Windows IOCP semantics where N posted items wake N threads

    Event Loop Structure (do_one)
    -----------------------------
    1. Lock mutex, try to pop handler from queue
    2. If got handler: execute it (unlocked), return
    3. If queue empty and no reactor running: become reactor
       - Run epoll_wait (unlocked), queue I/O completions, loop back
    4. If queue empty and reactor running: wait on condvar for work

    The task_running_ flag ensures only one thread owns epoll_wait().
    After the reactor queues I/O completions, it loops back to try getting
    a handler, giving priority to handler execution over more I/O polling.

    Signaling State (state_)
    ------------------------
    The state_ variable encodes two pieces of information:
    - Bit 0: signaled flag (1 = signaled, persists until cleared)
    - Upper bits: waiter count (each waiter adds 2 before blocking)

    This allows efficient coordination:
    - Signalers only call notify when waiters exist (state_ > 1)
    - Waiters check if already signaled before blocking (fast-path)

    Wake Coordination (wake_one_thread_and_unlock)
    ----------------------------------------------
    When posting work:
    - If waiters exist (state_ > 1): signal and notify_one()
    - Else if reactor running: interrupt via eventfd write
    - Else: no-op (thread will find work when it checks queue)

    This avoids waking threads unnecessarily. With cascading wakes,
    each handler execution wakes at most one additional thread if
    more work exists in the queue.

    Work Counting
    -------------
    outstanding_work_ tracks pending operations. When it hits zero, run()
    returns. Each operation increments on start, decrements on completion.

    Timer Integration
    -----------------
    Timers are handled by timer_service. The reactor adjusts epoll_wait
    timeout to wake for the nearest timer expiry. When a new timer is
    scheduled earlier than current, timer_service calls interrupt_reactor()
    to re-evaluate the timeout.
*/

namespace boost::corosio::detail {

struct scheduler_context
{
    epoll_scheduler const* key;
    scheduler_context* next;
    op_queue private_queue;
    long private_outstanding_work;
    int inline_budget;
    int inline_budget_max;
    bool unassisted;

    scheduler_context(epoll_scheduler const* k, scheduler_context* n)
        : key(k)
        , next(n)
        , private_outstanding_work(0)
        , inline_budget(0)
        , inline_budget_max(2)
        , unassisted(false)
    {
    }
};

namespace {

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        epoll_scheduler const* ctx) noexcept
        : frame_(ctx, context_stack.get())
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        if (!frame_.private_queue.empty())
            frame_.key->drain_thread_queue(frame_.private_queue, frame_.private_outstanding_work);
        context_stack.set(frame_.next);
    }
};

scheduler_context*
find_context(epoll_scheduler const* self) noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == self)
            return c;
    return nullptr;
}

} // namespace

void
epoll_scheduler::
reset_inline_budget() const noexcept
{
    if (auto* ctx = find_context(this))
    {
        // Cap when no other thread absorbed queued work. A moderate
        // cap (4) amortizes scheduling for small buffers while avoiding
        // bursty I/O that fills socket buffers and stalls large transfers.
        if (ctx->unassisted)
        {
            ctx->inline_budget_max = 4;
            ctx->inline_budget = 4;
            return;
        }
        // Ramp up when previous cycle fully consumed budget.
        // Reset on partial consumption (EAGAIN hit or peer got scheduled).
        if (ctx->inline_budget == 0)
            ctx->inline_budget_max = (std::min)(ctx->inline_budget_max * 2, 16);
        else if (ctx->inline_budget < ctx->inline_budget_max)
            ctx->inline_budget_max = 2;
        ctx->inline_budget = ctx->inline_budget_max;
    }
}

bool
epoll_scheduler::
try_consume_inline_budget() const noexcept
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
descriptor_state::
operator()()
{
    is_enqueued_.store(false, std::memory_order_relaxed);

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
    if (ev & EPOLLERR)
    {
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        if (err == 0)
            err = EIO;
    }

    {
        std::lock_guard lock(mutex);
        if (ev & EPOLLIN)
        {
            if (read_op)
            {
                auto* rd = read_op;
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
                    read_op = nullptr;
                    local_ops.push(rd);
                }
            }
            else
            {
                read_ready = true;
            }
        }
        if (ev & EPOLLOUT)
        {
            bool had_write_op = (connect_op || write_op);
            if (connect_op)
            {
                auto* cn = connect_op;
                if (err)
                    cn->complete(err, 0);
                else
                    cn->perform_io();
                connect_op = nullptr;
                local_ops.push(cn);
            }
            if (write_op)
            {
                auto* wr = write_op;
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
                    write_op = nullptr;
                    local_ops.push(wr);
                }
            }
            if (!had_write_op)
                write_ready = true;
        }
        if (err)
        {
            if (read_op)
            {
                read_op->complete(err, 0);
                local_ops.push(std::exchange(read_op, nullptr));
            }
            if (write_op)
            {
                write_op->complete(err, 0);
                local_ops.push(std::exchange(write_op, nullptr));
            }
            if (connect_op)
            {
                connect_op->complete(err, 0);
                local_ops.push(std::exchange(connect_op, nullptr));
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

epoll_scheduler::
epoll_scheduler(
    capy::execution_context& ctx,
    int)
    : epoll_fd_(-1)
    , event_fd_(-1)
    , timer_fd_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
    , task_running_{false}
    , task_interrupted_(false)
    , state_(0)
{
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        detail::throw_system_error(make_err(errno), "epoll_create1");

    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0)
    {
        int errn = errno;
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "eventfd");
    }

    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0)
    {
        int errn = errno;
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "timerfd_create");
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0)
    {
        int errn = errno;
        ::close(timer_fd_);
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "epoll_ctl");
    }

    epoll_event timer_ev{};
    timer_ev.events = EPOLLIN | EPOLLERR;
    timer_ev.data.ptr = &timer_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &timer_ev) < 0)
    {
        int errn = errno;
        ::close(timer_fd_);
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "epoll_ctl (timerfd)");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) {
                auto* self = static_cast<epoll_scheduler*>(p);
                self->timerfd_stale_.store(true, std::memory_order_release);
                if (self->task_running_.load(std::memory_order_acquire))
                    self->interrupt_reactor();
            }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);

    // Push task sentinel to interleave reactor runs with handler execution
    completed_ops_.push(&task_op_);
}

epoll_scheduler::
~epoll_scheduler()
{
    if (timer_fd_ >= 0)
        ::close(timer_fd_);
    if (event_fd_ >= 0)
        ::close(event_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

void
epoll_scheduler::
shutdown()
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

    if (event_fd_ >= 0)
        interrupt_reactor();
}

void
epoll_scheduler::
post(std::coroutine_handle<> h) const
{
    struct post_handler final
        : scheduler_op
    {
        std::coroutine_handle<> h_;

        explicit
        post_handler(std::coroutine_handle<> h)
            : h_(h)
        {
        }

        ~post_handler() = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
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
epoll_scheduler::
post(scheduler_op* h) const
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

void
epoll_scheduler::
on_work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
epoll_scheduler::
on_work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

bool
epoll_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
epoll_scheduler::
stop()
{
    std::unique_lock lock(mutex_);
    if (!stopped_)
    {
        stopped_ = true;
        signal_all(lock);
        interrupt_reactor();
    }
}

bool
epoll_scheduler::
stopped() const noexcept
{
    std::unique_lock lock(mutex_);
    return stopped_;
}

void
epoll_scheduler::
restart()
{
    std::unique_lock lock(mutex_);
    stopped_ = false;
}

std::size_t
epoll_scheduler::
run()
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
epoll_scheduler::
run_one()
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
epoll_scheduler::
wait_one(long usec)
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
epoll_scheduler::
poll()
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
epoll_scheduler::
poll_one()
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
epoll_scheduler::
register_descriptor(int fd, descriptor_state* desc) const
{
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP;
    ev.data.ptr = desc;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        detail::throw_system_error(make_err(errno), "epoll_ctl (register)");

    desc->registered_events = ev.events;
    desc->fd = fd;
    desc->scheduler_ = this;

    std::lock_guard lock(desc->mutex);
    desc->read_ready = false;
    desc->write_ready = false;
}

void
epoll_scheduler::
deregister_descriptor(int fd) const
{
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void
epoll_scheduler::
work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
epoll_scheduler::
work_finished() const noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Last work item completed - wake all threads so they can exit.
        // signal_all() wakes threads waiting on the condvar.
        // interrupt_reactor() wakes the reactor thread blocked in epoll_wait().
        // Both are needed because they target different blocking mechanisms.
        std::unique_lock lock(mutex_);
        signal_all(lock);
        if (task_running_.load(std::memory_order_relaxed) && !task_interrupted_)
        {
            task_interrupted_ = true;
            lock.unlock();
            interrupt_reactor();
        }
    }
}

void
epoll_scheduler::
compensating_work_started() const noexcept
{
    auto* ctx = find_context(this);
    if (ctx)
        ++ctx->private_outstanding_work;
}

void
epoll_scheduler::
drain_thread_queue(op_queue& queue, long count) const
{
    // Note: outstanding_work_ was already incremented when posting
    std::unique_lock lock(mutex_);
    completed_ops_.splice(queue);
    if (count > 0)
        maybe_unlock_and_signal_one(lock);
}

void
epoll_scheduler::
post_deferred_completions(op_queue& ops) const
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
epoll_scheduler::
interrupt_reactor() const
{
    // Only write if not already armed to avoid redundant writes
    bool expected = false;
    if (eventfd_armed_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        std::uint64_t val = 1;
        [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
    }
}

void
epoll_scheduler::
signal_all(std::unique_lock<std::mutex>&) const
{
    state_ |= 1;
    cond_.notify_all();
}

bool
epoll_scheduler::
maybe_unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const
{
    state_ |= 1;
    if (state_ > 1)
    {
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    return false;
}

bool
epoll_scheduler::
unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const
{
    state_ |= 1;
    bool have_waiters = state_ > 1;
    lock.unlock();
    if (have_waiters)
        cond_.notify_one();
    return have_waiters;
}

void
epoll_scheduler::
clear_signal() const
{
    state_ &= ~std::size_t(1);
}

void
epoll_scheduler::
wait_for_signal(std::unique_lock<std::mutex>& lock) const
{
    while ((state_ & 1) == 0)
    {
        state_ += 2;
        cond_.wait(lock);
        state_ -= 2;
    }
}

void
epoll_scheduler::
wait_for_signal_for(
    std::unique_lock<std::mutex>& lock,
    long timeout_us) const
{
    if ((state_ & 1) == 0)
    {
        state_ += 2;
        cond_.wait_for(lock, std::chrono::microseconds(timeout_us));
        state_ -= 2;
    }
}

void
epoll_scheduler::
wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const
{
    if (maybe_unlock_and_signal_one(lock))
        return;

    if (task_running_.load(std::memory_order_relaxed) && !task_interrupted_)
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
    epoll_scheduler const* scheduler;
    std::unique_lock<std::mutex>* lock;
    scheduler_context* ctx;

    ~work_cleanup()
    {
        if (ctx)
        {
            long produced = ctx->private_outstanding_work;
            if (produced > 1)
                scheduler->outstanding_work_.fetch_add(produced - 1, std::memory_order_relaxed);
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
    epoll_scheduler const* scheduler;
    std::unique_lock<std::mutex>* lock;
    scheduler_context* ctx;

    ~task_cleanup()
    {
        if (!ctx)
            return;

        if (ctx->private_outstanding_work > 0)
        {
            scheduler->outstanding_work_.fetch_add(
                ctx->private_outstanding_work, std::memory_order_relaxed);
            ctx->private_outstanding_work = 0;
        }

        if (!ctx->private_queue.empty())
        {
            if (!lock->owns_lock())
                lock->lock();
            scheduler->completed_ops_.splice(ctx->private_queue);
        }
    }
};

void
epoll_scheduler::
update_timerfd() const
{
    auto nearest = timer_svc_->nearest_expiry();

    itimerspec ts{};
    int flags = 0;

    if (nearest == timer_service::time_point::max())
    {
        // No timers - disarm by setting to 0 (relative)
    }
    else
    {
        auto now = std::chrono::steady_clock::now();
        if (nearest <= now)
        {
            // Use 1ns instead of 0 - zero disarms the timerfd
            ts.it_value.tv_nsec = 1;
        }
        else
        {
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                nearest - now).count();
            ts.it_value.tv_sec = nsec / 1000000000;
            ts.it_value.tv_nsec = nsec % 1000000000;
            // Ensure non-zero to avoid disarming if duration rounds to 0
            if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
                ts.it_value.tv_nsec = 1;
        }
    }

    if (::timerfd_settime(timer_fd_, flags, &ts, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "timerfd_settime");
}

void
epoll_scheduler::
run_task(std::unique_lock<std::mutex>& lock, scheduler_context* ctx)
{
    int timeout_ms = task_interrupted_ ? 0 : -1;

    if (lock.owns_lock())
        lock.unlock();

    task_cleanup on_exit{this, &lock, ctx};

    // Flush deferred timerfd programming before blocking
    if (timerfd_stale_.exchange(false, std::memory_order_acquire))
        update_timerfd();

    // Event loop runs without mutex held
    epoll_event events[128];
    int nfds = ::epoll_wait(epoll_fd_, events, 128, timeout_ms);

    if (nfds < 0 && errno != EINTR)
        detail::throw_system_error(make_err(errno), "epoll_wait");

    bool check_timers = false;
    op_queue local_ops;

    // Process events without holding the mutex
    for (int i = 0; i < nfds; ++i)
    {
        if (events[i].data.ptr == nullptr)
        {
            std::uint64_t val;
            [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
            eventfd_armed_.store(false, std::memory_order_relaxed);
            continue;
        }

        if (events[i].data.ptr == &timer_fd_)
        {
            std::uint64_t expirations;
            [[maybe_unused]] auto r = ::read(timer_fd_, &expirations, sizeof(expirations));
            check_timers = true;
            continue;
        }

        // Deferred I/O: just set ready events and enqueue descriptor
        // No per-descriptor mutex locking in reactor hot path!
        auto* desc = static_cast<descriptor_state*>(events[i].data.ptr);
        desc->add_ready_events(events[i].events);

        // Only enqueue if not already enqueued
        bool expected = false;
        if (desc->is_enqueued_.compare_exchange_strong(expected, true,
                std::memory_order_release, std::memory_order_relaxed))
        {
            local_ops.push(desc);
        }
    }

    // Process timers only when timerfd fires
    if (check_timers)
    {
        timer_svc_->process_expired();
        update_timerfd();
    }

    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);
}

std::size_t
epoll_scheduler::
do_one(std::unique_lock<std::mutex>& lock, long timeout_us, scheduler_context* ctx)
{
    for (;;)
    {
        if (stopped_)
            return 0;

        scheduler_op* op = completed_ops_.pop();

        // Handle reactor sentinel - time to poll for I/O
        if (op == &task_op_)
        {
            bool more_handlers = !completed_ops_.empty();

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
            task_running_.store(true, std::memory_order_release);

            if (more_handlers)
                unlock_and_signal_one(lock);

            run_task(lock, ctx);

            task_running_.store(false, std::memory_order_relaxed);
            completed_ops_.push(&task_op_);
            continue;
        }

        // Handle operation
        if (op != nullptr)
        {
            bool more = !completed_ops_.empty();

            if (more)
                ctx->unassisted = !unlock_and_signal_one(lock);
            else
            {
                ctx->unassisted = false;
                lock.unlock();
            }

            work_cleanup on_exit{this, &lock, ctx};

            (*op)();
            return 1;
        }

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
