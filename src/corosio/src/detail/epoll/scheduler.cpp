//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include "src/detail/epoll/scheduler.hpp"
#include "src/detail/epoll/op.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
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
};

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        epoll_scheduler const* ctx) noexcept
        : frame_{ctx, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        context_stack.set(frame_.next);
    }
};

} // namespace

epoll_scheduler::
epoll_scheduler(
    capy::execution_context& ctx,
    int)
    : epoll_fd_(-1)
    , event_fd_(-1)
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

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0)
    {
        int errn = errno;
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "epoll_ctl");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) { static_cast<epoll_scheduler*>(p)->interrupt_reactor(); }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);
}

epoll_scheduler::
~epoll_scheduler()
{
    if (event_fd_ >= 0)
        ::close(event_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

void
epoll_scheduler::
shutdown()
{
    std::unique_lock lock(mutex_);
    shutdown_ = true;

    while (auto* h = completed_ops_.pop())
    {
        lock.unlock();
        h->destroy();
        lock.lock();
    }

    // Wake all waiting threads so they can exit
    wakeup_event_.notify_all();
    outstanding_work_.store(0, std::memory_order_release);
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
            h.resume();
        }

        void destroy() override
        {
            delete this;
        }
    };

    auto ph = std::make_unique<post_handler>(h);
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(ph.release());
    wake_one_thread_and_unlock(lock);
}

void
epoll_scheduler::
post(scheduler_op* h) const
{
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
register_fd(int fd, epoll_op* op, std::uint32_t events) const
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = op;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        detail::throw_system_error(make_err(errno), "epoll_ctl ADD");
}

void
epoll_scheduler::
modify_fd(int fd, epoll_op* op, std::uint32_t events) const
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = op;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        detail::throw_system_error(make_err(errno), "epoll_ctl MOD");
}

void
epoll_scheduler::
unregister_fd(int fd) const
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
interrupt_reactor() const
{
    std::uint64_t val = 1;
    [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
}

void
epoll_scheduler::
wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const
{
    if (idle_thread_count_ > 0)
    {
        // Idle worker exists - wake it via condvar
        wakeup_event_.notify_one();
        lock.unlock();
    }
    else if (reactor_running_ && !reactor_interrupted_)
    {
        // No idle workers but reactor is running - interrupt it so it
        // can re-check the queue after processing current epoll events
        reactor_interrupted_ = true;
        lock.unlock();
        interrupt_reactor();
    }
    else
    {
        // No one to wake - either reactor will pick up work when it
        // re-checks queue, or next thread to call run() will get it
        lock.unlock();
    }
}

struct work_guard
{
    epoll_scheduler const* self;
    ~work_guard() { self->work_finished(); }
};

long
epoll_scheduler::
calculate_timeout(long requested_timeout_us) const
{
    if (requested_timeout_us == 0)
        return 0;

    auto nearest = timer_svc_->nearest_expiry();
    if (nearest == timer_service::time_point::max())
        return requested_timeout_us;

    auto now = std::chrono::steady_clock::now();
    if (nearest <= now)
        return 0;

    auto timer_timeout_us = std::chrono::duration_cast<std::chrono::microseconds>(
        nearest - now).count();

    if (requested_timeout_us < 0)
        return static_cast<long>(timer_timeout_us);

    return static_cast<long>((std::min)(
        static_cast<long long>(requested_timeout_us),
        static_cast<long long>(timer_timeout_us)));
}

void
epoll_scheduler::
run_reactor(std::unique_lock<std::mutex>& lock)
{
    // Calculate timeout considering timers, use 0 if interrupted
    long effective_timeout_us = reactor_interrupted_ ? 0 : calculate_timeout(-1);

    int timeout_ms;
    if (effective_timeout_us < 0)
        timeout_ms = -1;
    else if (effective_timeout_us == 0)
        timeout_ms = 0;
    else
        timeout_ms = static_cast<int>((effective_timeout_us + 999) / 1000);

    lock.unlock();

    epoll_event events[64];
    int nfds = ::epoll_wait(epoll_fd_, events, 64, timeout_ms);
    int saved_errno = errno;  // Save before process_expired() may overwrite

    // Process timers outside the lock - timer completions may call post()
    // which needs to acquire the lock
    timer_svc_->process_expired();

    if (nfds < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "epoll_wait");

    // Process I/O completions - these become handlers in the queue
    // Must re-acquire lock before modifying completed_ops_
    lock.lock();

    int completions_queued = 0;
    for (int i = 0; i < nfds; ++i)
    {
        if (events[i].data.ptr == nullptr)
        {
            // eventfd interrupt - just drain it
            std::uint64_t val;
            [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
            continue;
        }

        auto* op = static_cast<epoll_op*>(events[i].data.ptr);

        bool was_registered = op->registered.exchange(false, std::memory_order_acq_rel);
        if (!was_registered)
            continue;

        unregister_fd(op->fd);

        if (events[i].events & (EPOLLERR | EPOLLHUP))
        {
            int errn = 0;
            socklen_t len = sizeof(errn);
            if (::getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
                errn = errno;
            if (errn == 0)
                errn = EIO;
            op->complete(errn, 0);
        }
        else
        {
            op->perform_io();
        }

        completed_ops_.push(op);
        ++completions_queued;
    }

    // Wake idle workers if we queued I/O completions
    if (completions_queued > 0)
    {
        if (completions_queued >= idle_thread_count_)
            wakeup_event_.notify_all();
        else
            for (int i = 0; i < completions_queued; ++i)
                wakeup_event_.notify_one();
    }
}

std::size_t
epoll_scheduler::
do_one(long timeout_us)
{
    std::unique_lock lock(mutex_);

    using clock = std::chrono::steady_clock;
    auto deadline = (timeout_us > 0)
        ? clock::now() + std::chrono::microseconds(timeout_us)
        : clock::time_point{};

    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        // Try to get a handler from the queue
        scheduler_op* op = completed_ops_.pop();

        if (op != nullptr)
        {
            // Got a handler - execute it
            lock.unlock();
            work_guard g{this};
            (*op)();
            return 1;
        }

        // Queue is empty - check if we should become reactor or wait
        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        if (timeout_us == 0)
            return 0;  // Non-blocking poll

        // Check if timeout has expired (for positive timeout_us)
        long remaining_us = timeout_us;
        if (timeout_us > 0)
        {
            auto now = clock::now();
            if (now >= deadline)
                return 0;
            remaining_us = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now).count();
        }

        if (!reactor_running_)
        {
            // No reactor running and queue empty - become the reactor
            reactor_running_ = true;
            reactor_interrupted_ = false;

            run_reactor(lock);

            reactor_running_ = false;
            // Loop back to check for handlers that reactor may have queued
            continue;
        }

        // Reactor is running in another thread - wait for work on condvar
        ++idle_thread_count_;
        if (timeout_us < 0)
            wakeup_event_.wait(lock);
        else
            wakeup_event_.wait_for(lock, std::chrono::microseconds(remaining_us));
        --idle_thread_count_;
    }
}

} // namespace boost::corosio::detail

#endif
