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
#include "src/detail/make_err.hpp"
#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <limits>

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
    - OTHER threads wait on wakeup_event_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()
    - This matches Windows IOCP semantics where N posted items wake N threads

    Event Loop Structure (do_one)
    -----------------------------
    1. Lock mutex, try to pop handler from queue
    2. If got handler: execute it (unlocked), return
    3. If queue empty and no reactor running: become reactor
       - Run epoll_wait (unlocked), queue I/O completions, loop back
    4. If queue empty and reactor running: wait on condvar for work

    The reactor_running_ flag ensures only one thread owns epoll_wait().
    After the reactor queues I/O completions, it loops back to try getting
    a handler, giving priority to handler execution over more I/O polling.

    Wake Coordination (wake_one_thread_and_unlock)
    ----------------------------------------------
    When posting work:
    - If idle threads exist: notify_one() wakes exactly one worker
    - Else if reactor running: interrupt via eventfd write
    - Else: no-op (thread will find work when it checks queue)

    This is critical for matching IOCP behavior. With the old model, posting
    N handlers would wake all threads (thundering herd). Now each post()
    wakes at most one thread, and that thread handles exactly one item.

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

namespace {

struct scheduler_context
{
    epoll_scheduler const* key;
    scheduler_context* next;
    op_queue private_queue;
    long private_outstanding_work;

    scheduler_context(epoll_scheduler const* k, scheduler_context* n)
        : key(k)
        , next(n)
        , private_outstanding_work(0)
    {
    }
};

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
    , reactor_running_(false)
    , reactor_interrupted_(false)
    , idle_thread_count_(0)
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
            [](void* p) { static_cast<epoll_scheduler*>(p)->update_timerfd(); }));

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
    }

    outstanding_work_.store(0, std::memory_order_release);

    if (event_fd_ >= 0)
        interrupt_reactor();

    wakeup_event_.notify_all();
}

void
epoll_scheduler::
post(capy::coro h) const
{
    struct post_handler final
        : scheduler_op
    {
        capy::coro h_;

        explicit
        post_handler(capy::coro h)
            : h_(h)
        {
        }

        ~post_handler() = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
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
    bool expected = false;
    if (stopped_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        // Wake all threads so they notice stopped_ and exit
        {
            std::lock_guard lock(mutex_);
            wakeup_event_.notify_all();
        }
        interrupt_reactor();
    }
}

bool
epoll_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
epoll_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
epoll_scheduler::
run()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(-1))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

std::size_t
epoll_scheduler::
run_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(-1);
}

std::size_t
epoll_scheduler::
wait_one(long usec)
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(usec);
}

std::size_t
epoll_scheduler::
poll()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(0))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

std::size_t
epoll_scheduler::
poll_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(0);
}

void
epoll_scheduler::
register_descriptor(int fd, descriptor_data* desc) const
{
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP;
    ev.data.ptr = desc;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        detail::throw_system_error(make_err(errno), "epoll_ctl (register)");

    desc->registered_events = ev.events;
    desc->is_registered = true;
    desc->fd = fd;
    desc->read_ready.store(false, std::memory_order_relaxed);
    desc->write_ready.store(false, std::memory_order_relaxed);
}

void
epoll_scheduler::
update_descriptor_events(int, descriptor_data*, std::uint32_t) const
{
    // Provides memory fence for operation pointer visibility across threads
    std::atomic_thread_fence(std::memory_order_seq_cst);
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
        // notify_all() wakes threads waiting on the condvar.
        // interrupt_reactor() wakes the reactor thread blocked in epoll_wait().
        // Both are needed because they target different blocking mechanisms.
        std::unique_lock lock(mutex_);
        wakeup_event_.notify_all();
        if (reactor_running_ && !reactor_interrupted_)
        {
            reactor_interrupted_ = true;
            lock.unlock();
            interrupt_reactor();
        }
    }
}

void
epoll_scheduler::
drain_thread_queue(op_queue& queue, long count) const
{
    // Note: outstanding_work_ was already incremented when posting
    std::lock_guard lock(mutex_);
    completed_ops_.splice(queue);
    if (count > 0)
        wakeup_event_.notify_all();
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
wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const
{
    if (idle_thread_count_ > 0)
    {
        wakeup_event_.notify_one();
        lock.unlock();
    }
    else if (reactor_running_ && !reactor_interrupted_)
    {
        reactor_interrupted_ = true;
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
                lock->unlock();
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
run_reactor(std::unique_lock<std::mutex>& lock)
{
    auto* ctx = find_context(this);
    int timeout_ms = reactor_interrupted_ ? 0 : -1;

    lock.unlock();

    // Flush private work count when reactor completes
    task_cleanup on_exit{this, ctx};
    (void)on_exit;

    // --- Event loop runs WITHOUT the mutex (like Asio) ---

    epoll_event events[128];
    int nfds = ::epoll_wait(epoll_fd_, events, 128, timeout_ms);
    int saved_errno = errno;

    if (nfds < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "epoll_wait");

    bool check_timers = false;
    op_queue local_ops;
    int completions_queued = 0;

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

        auto* desc = static_cast<descriptor_data*>(events[i].data.ptr);
        std::uint32_t ev = events[i].events;
        int err = 0;

        if (ev & (EPOLLERR | EPOLLHUP))
        {
            socklen_t len = sizeof(err);
            if (::getsockopt(desc->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                err = errno;
            if (err == 0)
                err = EIO;
        }

        if (ev & EPOLLIN)
        {
            auto* op = desc->read_op.exchange(nullptr, std::memory_order_acq_rel);
            if (op)
            {
                if (err)
                {
                    op->complete(err, 0);
                    local_ops.push(op);
                    ++completions_queued;
                }
                else
                {
                    op->perform_io();
                    if (op->errn == EAGAIN || op->errn == EWOULDBLOCK)
                    {
                        op->errn = 0;
                        desc->read_op.store(op, std::memory_order_release);
                    }
                    else
                    {
                        local_ops.push(op);
                        ++completions_queued;
                    }
                }
            }
            else
            {
                desc->read_ready.store(true, std::memory_order_release);
            }
        }

        if (ev & EPOLLOUT)
        {
            auto* conn_op = desc->connect_op.exchange(nullptr, std::memory_order_acq_rel);
            if (conn_op)
            {
                if (err)
                {
                    conn_op->complete(err, 0);
                    local_ops.push(conn_op);
                    ++completions_queued;
                }
                else
                {
                    conn_op->perform_io();
                    if (conn_op->errn == EAGAIN || conn_op->errn == EWOULDBLOCK)
                    {
                        conn_op->errn = 0;
                        desc->connect_op.store(conn_op, std::memory_order_release);
                    }
                    else
                    {
                        local_ops.push(conn_op);
                        ++completions_queued;
                    }
                }
            }

            auto* write_op = desc->write_op.exchange(nullptr, std::memory_order_acq_rel);
            if (write_op)
            {
                if (err)
                {
                    write_op->complete(err, 0);
                    local_ops.push(write_op);
                    ++completions_queued;
                }
                else
                {
                    write_op->perform_io();
                    if (write_op->errn == EAGAIN || write_op->errn == EWOULDBLOCK)
                    {
                        write_op->errn = 0;
                        desc->write_op.store(write_op, std::memory_order_release);
                    }
                    else
                    {
                        local_ops.push(write_op);
                        ++completions_queued;
                    }
                }
            }

            if (!conn_op && !write_op)
                desc->write_ready.store(true, std::memory_order_release);
        }

        if (err && !(ev & (EPOLLIN | EPOLLOUT)))
        {
            auto* read_op = desc->read_op.exchange(nullptr, std::memory_order_acq_rel);
            if (read_op)
            {
                read_op->complete(err, 0);
                local_ops.push(read_op);
                ++completions_queued;
            }

            auto* write_op = desc->write_op.exchange(nullptr, std::memory_order_acq_rel);
            if (write_op)
            {
                write_op->complete(err, 0);
                local_ops.push(write_op);
                ++completions_queued;
            }

            auto* conn_op = desc->connect_op.exchange(nullptr, std::memory_order_acq_rel);
            if (conn_op)
            {
                conn_op->complete(err, 0);
                local_ops.push(conn_op);
                ++completions_queued;
            }
        }
    }

    // Process timers only when timerfd fires (like Asio's check_timers pattern)
    if (check_timers)
    {
        timer_svc_->process_expired();
        update_timerfd();
    }

    // --- Acquire mutex only for queue operations ---
    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);

    // Drain private queue to global (work count handled by task_cleanup)
    if (ctx && !ctx->private_queue.empty())
    {
        completions_queued += ctx->private_outstanding_work;
        completed_ops_.splice(ctx->private_queue);
    }

    // Only wake threads that are actually idle, and only as many as we have work
    if (completions_queued > 0 && idle_thread_count_ > 0)
    {
        int threads_to_wake = (std::min)(completions_queued, idle_thread_count_);
        for (int i = 0; i < threads_to_wake; ++i)
            wakeup_event_.notify_one();
    }
}

std::size_t
epoll_scheduler::
do_one(long timeout_us)
{
    std::unique_lock lock(mutex_);

    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        scheduler_op* op = completed_ops_.pop();

        if (op == &task_op_)
        {
            // Check both global queue and private queue for pending handlers
            auto* ctx = find_context(this);
            bool more_handlers = !completed_ops_.empty() ||
                (ctx && !ctx->private_queue.empty());

            if (!more_handlers)
            {
                if (outstanding_work_.load(std::memory_order_acquire) == 0)
                {
                    completed_ops_.push(&task_op_);
                    return 0;
                }
                if (timeout_us == 0)
                {
                    completed_ops_.push(&task_op_);
                    return 0;
                }
            }

            reactor_interrupted_ = more_handlers || timeout_us == 0;
            reactor_running_ = true;

            if (more_handlers && idle_thread_count_ > 0)
                wakeup_event_.notify_one();

            run_reactor(lock);

            reactor_running_ = false;
            completed_ops_.push(&task_op_);
            continue;
        }

        if (op != nullptr)
        {
            auto* ctx = find_context(this);
            lock.unlock();

            work_cleanup on_exit{this, &lock, ctx};
            (void)on_exit;

            (*op)();
            return 1;
        }

        // Drain private queue before blocking, flush work count to global
        if (auto* ctx = find_context(this))
        {
            if (ctx->private_outstanding_work > 0)
            {
                outstanding_work_.fetch_add(
                    ctx->private_outstanding_work, std::memory_order_relaxed);
                ctx->private_outstanding_work = 0;
            }
            if (!ctx->private_queue.empty())
            {
                completed_ops_.splice(ctx->private_queue);
                continue;
            }
        }

        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        if (timeout_us == 0)
            return 0;

        ++idle_thread_count_;
        if (timeout_us < 0)
            wakeup_event_.wait(lock);
        else
            wakeup_event_.wait_for(lock, std::chrono::microseconds(timeout_us));
        --idle_thread_count_;
    }
}

} // namespace boost::corosio::detail

#endif
