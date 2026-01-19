//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef _WIN32

#include "src/detail/posix_scheduler.hpp"
#include "src/detail/posix_op.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/core/thread_local_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost {
namespace corosio {
namespace detail {

namespace {

struct scheduler_context
{
    posix_scheduler const* key;
    scheduler_context* next;
};

capy::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        posix_scheduler const* ctx) noexcept
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

posix_scheduler::
posix_scheduler(
    capy::execution_context& ctx,
    int)
    : epoll_fd_(-1)
    , event_fd_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
{
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        detail::throw_system_error(
            system::error_code(errno, system::system_category()),
            "epoll_create1");

    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0)
    {
        int err = errno;
        ::close(epoll_fd_);
        detail::throw_system_error(
            system::error_code(err, system::system_category()),
            "eventfd");
    }

    // data.ptr = nullptr distinguishes wakeup events from I/O completions
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0)
    {
        int err = errno;
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(
            system::error_code(err, system::system_category()),
            "epoll_ctl");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) { static_cast<posix_scheduler*>(p)->wakeup(); }));
}

posix_scheduler::
~posix_scheduler()
{
    if (event_fd_ >= 0)
        ::close(event_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

void
posix_scheduler::
shutdown()
{
    std::unique_lock lock(mutex_);
    shutdown_ = true;

    // Drain all completed operations without invoking handlers
    while (auto* h = completed_ops_.pop())
    {
        lock.unlock();
        h->destroy();
        lock.lock();
    }

    // Reset outstanding work count - any pending I/O operations
    // will be cleaned up when their owning objects are destroyed
    outstanding_work_.store(0, std::memory_order_release);
}

void
posix_scheduler::
post(capy::any_coro h) const
{
    struct post_handler final
        : scheduler_op
    {
        capy::any_coro h_;

        explicit
        post_handler(capy::any_coro h)
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
posix_scheduler::
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
posix_scheduler::
on_work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
posix_scheduler::
on_work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

bool
posix_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
posix_scheduler::
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
posix_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
posix_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
posix_scheduler::
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
posix_scheduler::
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
posix_scheduler::
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
posix_scheduler::
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
posix_scheduler::
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
posix_scheduler::
register_fd(int fd, posix_op* op, std::uint32_t events) const
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = op;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        detail::throw_system_error(
            system::error_code(errno, system::system_category()),
            "epoll_ctl ADD");
    }
}

void
posix_scheduler::
modify_fd(int fd, posix_op* op, std::uint32_t events) const
{
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = op;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        detail::throw_system_error(
            system::error_code(errno, system::system_category()),
            "epoll_ctl MOD");
    }
}

void
posix_scheduler::
unregister_fd(int fd) const
{
    // EPOLL_CTL_DEL ignores the event parameter (can be NULL on Linux 2.6.9+)
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void
posix_scheduler::
work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
posix_scheduler::
work_finished() const noexcept
{
    outstanding_work_.fetch_sub(1, std::memory_order_acq_rel);
}

void
posix_scheduler::
wakeup() const
{
    // Write cannot fail: eventfd counter won't overflow with single increments
    std::uint64_t val = 1;
    [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
}

// RAII guard - work_finished called even if handler throws
struct work_guard
{
    posix_scheduler const* self;
    ~work_guard() { self->work_finished(); }
};

long
posix_scheduler::
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
posix_scheduler::
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

        int timeout_ms;
        if (effective_timeout_us < 0)
            timeout_ms = -1;
        else if (effective_timeout_us == 0)
            timeout_ms = 0;
        else
            timeout_ms = static_cast<int>((effective_timeout_us + 999) / 1000);

        epoll_event events[64];
        int nfds = ::epoll_wait(epoll_fd_, events, 64, timeout_ms);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                // EINTR: retry for infinite waits, return for timed waits
                if (timeout_us < 0)
                    continue;
                return 0;
            }
            detail::throw_system_error(
                system::error_code(errno, system::system_category()),
                "epoll_wait");
        }

        // May dispatch timer handlers inline
        timer_svc_->process_expired();

        for (int i = 0; i < nfds; ++i)
        {
            if (events[i].data.ptr == nullptr)
            {
                // Drain eventfd; read cannot fail since epoll signaled readiness
                std::uint64_t val;
                [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
                continue;
            }

            auto* op = static_cast<posix_op*>(events[i].data.ptr);

            // One-shot: unregister before I/O
            unregister_fd(op->fd);

            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                int err = 0;
                socklen_t len = sizeof(err);
                if (::getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                    err = errno;
                if (err == 0)
                    err = EIO;
                op->complete(err, 0);
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

        // Finite timeout: return on timeout; infinite: keep looping
        if (timeout_us >= 0)
            return 0;
    }
}

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
