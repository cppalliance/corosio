//
// Copyright (c) 2026 Cinar Gursoy
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)

#include "src/detail/kqueue/scheduler.hpp"
#include "src/detail/kqueue/op.hpp"
#include "src/detail/make_err.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/detail/thread_local_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
    kqueue Scheduler
    ================

    The scheduler is the heart of the I/O event loop. It multiplexes I/O
    readiness notifications from kqueue with a completion queue for operations
    that finished synchronously or were cancelled.

    Event Loop Structure (do_one)
    -----------------------------
    1. Check completion queue first (mutex-protected)
    2. If empty, call kevent with calculated timeout
    3. Process timer expirations
    4. For each ready fd, claim the operation and perform I/O
    5. Push completed operations to completion queue
    6. Pop one and invoke its handler

    The completion queue exists because handlers must run outside the kqueue
    processing loop. This allows handlers to safely start new operations
    on the same fd without corrupting iteration state.

    Wakeup Mechanism
    ----------------
    EVFILT_USER allows other threads (or cancel/post calls) to wake the
    event loop from kevent. We trigger it with NOTE_TRIGGER and identify
    it by checking filter == EVFILT_USER.

    Work Counting
    -------------
    outstanding_work_ tracks pending operations. When it hits zero, run()
    returns. This is how io_context knows there's nothing left to do.
    Each operation increments on start, decrements on completion.

    Timer Integration
    -----------------
    Timers are handled by timer_service. The scheduler adjusts kevent
    timeout to wake in time for the nearest timer expiry. When a new timer
    is scheduled earlier than current, timer_service calls wakeup() to
    re-evaluate the timeout.
*/

namespace boost {
namespace corosio {
namespace detail {

namespace {

struct scheduler_context
{
    kqueue_scheduler const* key;
    scheduler_context* next;
};

capy::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        kqueue_scheduler const* ctx) noexcept
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

kqueue_scheduler::
kqueue_scheduler(
    capy::execution_context& ctx,
    int)
    : kqueue_fd_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
{
    kqueue_fd_ = ::kqueue();
    if (kqueue_fd_ < 0)
        detail::throw_system_error(make_err(errno), "kqueue");

    // Register EVFILT_USER for wakeup (replaces eventfd on Linux)
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
    {
        int errn = errno;
        ::close(kqueue_fd_);
        detail::throw_system_error(make_err(errn), "kevent EVFILT_USER");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) { static_cast<kqueue_scheduler*>(p)->wakeup(); }));
}

kqueue_scheduler::
~kqueue_scheduler()
{
    if (kqueue_fd_ >= 0)
        ::close(kqueue_fd_);
}

void
kqueue_scheduler::
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

    outstanding_work_.store(0, std::memory_order_release);
}

void
kqueue_scheduler::
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

    auto* ph = new post_handler(h);
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard lock(mutex_);
        completed_ops_.push(ph);
    }
    wakeup();
}

void
kqueue_scheduler::
post(scheduler_op* h) const
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard lock(mutex_);
        completed_ops_.push(h);
    }
    wakeup();
}

void
kqueue_scheduler::
on_work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
kqueue_scheduler::
on_work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

bool
kqueue_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
kqueue_scheduler::
stop()
{
    bool expected = false;
    if (stopped_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        wakeup();
    }
}

bool
kqueue_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
kqueue_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
kqueue_scheduler::
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
kqueue_scheduler::
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
kqueue_scheduler::
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
kqueue_scheduler::
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
kqueue_scheduler::
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
kqueue_scheduler::
register_fd(int fd, kqueue_op* op, int16_t filter) const
{
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT | EV_CLEAR, 0, 0, op);
    if (::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "kevent EV_ADD");
}

void
kqueue_scheduler::
modify_fd(int fd, kqueue_op* op, int16_t filter) const
{
    // kqueue replaces on EV_ADD, same as register
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT | EV_CLEAR, 0, 0, op);
    if (::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "kevent EV_ADD (modify)");
}

void
kqueue_scheduler::
unregister_fd(int fd, int16_t filter) const
{
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
    // Ignore errors - fd may already be closed or not registered
    ::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
}

void
kqueue_scheduler::
work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
kqueue_scheduler::
work_finished() const noexcept
{
    outstanding_work_.fetch_sub(1, std::memory_order_acq_rel);
}

void
kqueue_scheduler::
wakeup() const
{
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    ::kevent(kqueue_fd_, &ev, 1, nullptr, 0, nullptr);
}

struct work_guard
{
    kqueue_scheduler const* self;
    ~work_guard() { self->work_finished(); }
};

long
kqueue_scheduler::
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

std::size_t
kqueue_scheduler::
do_one(long timeout_us)
{
    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        scheduler_op* h = nullptr;
        {
            std::lock_guard lock(mutex_);
            h = completed_ops_.pop();
        }

        if (h)
        {
            work_guard g{this};
            (*h)();
            return 1;
        }

        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        long effective_timeout_us = calculate_timeout(timeout_us);

        struct timespec ts;
        struct timespec* pts = nullptr;
        if (effective_timeout_us >= 0)
        {
            ts.tv_sec = effective_timeout_us / 1000000;
            ts.tv_nsec = (effective_timeout_us % 1000000) * 1000;
            pts = &ts;
        }

        struct kevent events[64];
        int nev = ::kevent(kqueue_fd_, nullptr, 0, events, 64, pts);

        if (nev < 0)
        {
            if (errno == EINTR)
            {
                if (timeout_us < 0)
                    continue;
                return 0;
            }
            detail::throw_system_error(make_err(errno), "kevent");
        }

        timer_svc_->process_expired();

        for (int i = 0; i < nev; ++i)
        {
            // Skip wakeup events (EVFILT_USER)
            if (events[i].filter == EVFILT_USER)
                continue;

            auto* op = static_cast<kqueue_op*>(events[i].udata);
            if (!op)
                continue;

            bool was_registered = op->registered.exchange(false, std::memory_order_acq_rel);
            if (!was_registered)
                continue;

            // Check for errors
            if (events[i].flags & (EV_EOF | EV_ERROR))
            {
                int errn = 0;
                if (events[i].flags & EV_ERROR)
                {
                    errn = static_cast<int>(events[i].data);
                }
                else
                {
                    // EV_EOF - check socket error
                    socklen_t len = sizeof(errn);
                    if (::getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
                        errn = errno;
                }
                if (errn == 0 && (events[i].flags & EV_EOF))
                {
                    // EOF on read is not an error, just zero bytes
                    op->perform_io();
                }
                else if (errn != 0)
                {
                    op->complete(errn, 0);
                }
                else
                {
                    op->perform_io();
                }
            }
            else
            {
                op->perform_io();
            }

            {
                std::lock_guard lock(mutex_);
                completed_ops_.push(op);
            }
        }

        if (stopped_.load(std::memory_order_acquire))
            return 0;

        {
            std::lock_guard lock(mutex_);
            h = completed_ops_.pop();
        }

        if (h)
        {
            work_guard g{this};
            (*h)();
            return 1;
        }

        if (timeout_us >= 0)
            return 0;
    }
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
