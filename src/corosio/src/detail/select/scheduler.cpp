//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//


#if !defined(_WIN32)

#include "src/detail/select/scheduler.hpp"
#include "src/detail/select/op.hpp"
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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    select Scheduler - Single Reactor Model
    =======================================

    This scheduler mirrors the epoll_scheduler design but uses select() instead
    of epoll for I/O multiplexing. The thread coordination strategy is identical:
    one thread becomes the "reactor" while others wait on a condition variable.

    Thread Model
    ------------
    - ONE thread runs select() at a time (the reactor thread)
    - OTHER threads wait on wakeup_event_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()

    Key Differences from epoll
    --------------------------
    - Uses self-pipe instead of eventfd for interruption (more portable)
    - fd_set rebuilding each iteration (O(n) vs O(1) for epoll)
    - FD_SETSIZE limit (~1024 fds on most systems)
    - Level-triggered only (no edge-triggered mode)

    Self-Pipe Pattern
    -----------------
    To interrupt a blocking select() call (e.g., when work is posted or a timer
    expires), we write a byte to pipe_fds_[1]. The read end pipe_fds_[0] is
    always in the read_fds set, so select() returns immediately. We drain the
    pipe to clear the readable state.

    fd-to-op Mapping
    ----------------
    We use an unordered_map<int, fd_state> to track which operations are
    registered for each fd. This allows O(1) lookup when select() returns
    ready fds. Each fd can have at most one read op and one write op registered.
*/

namespace boost::corosio::detail {

namespace {

struct scheduler_context
{
    select_scheduler const* key;
    scheduler_context* next;
};

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        select_scheduler const* ctx) noexcept
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

select_scheduler::
select_scheduler(
    capy::execution_context& ctx,
    int)
    : pipe_fds_{-1, -1}
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
    , max_fd_(-1)
    , reactor_running_(false)
    , reactor_interrupted_(false)
    , idle_thread_count_(0)
{
    // Create self-pipe for interrupting select()
    if (::pipe(pipe_fds_) < 0)
        detail::throw_system_error(make_err(errno), "pipe");

    // Set both ends to non-blocking and close-on-exec
    for (int i = 0; i < 2; ++i)
    {
        int flags = ::fcntl(pipe_fds_[i], F_GETFL, 0);
        if (flags == -1)
        {
            int errn = errno;
            ::close(pipe_fds_[0]);
            ::close(pipe_fds_[1]);
            detail::throw_system_error(make_err(errn), "fcntl F_GETFL");
        }
        if (::fcntl(pipe_fds_[i], F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int errn = errno;
            ::close(pipe_fds_[0]);
            ::close(pipe_fds_[1]);
            detail::throw_system_error(make_err(errn), "fcntl F_SETFL");
        }
        if (::fcntl(pipe_fds_[i], F_SETFD, FD_CLOEXEC) == -1)
        {
            int errn = errno;
            ::close(pipe_fds_[0]);
            ::close(pipe_fds_[1]);
            detail::throw_system_error(make_err(errn), "fcntl F_SETFD");
        }
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) { static_cast<select_scheduler*>(p)->interrupt_reactor(); }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);
}

select_scheduler::
~select_scheduler()
{
    if (pipe_fds_[0] >= 0)
        ::close(pipe_fds_[0]);
    if (pipe_fds_[1] >= 0)
        ::close(pipe_fds_[1]);
}

void
select_scheduler::
shutdown()
{
    {
        std::unique_lock lock(mutex_);
        shutdown_ = true;

        while (auto* h = completed_ops_.pop())
        {
            lock.unlock();
            h->destroy();
            lock.lock();
        }
    }

    outstanding_work_.store(0, std::memory_order_release);

    if (pipe_fds_[1] >= 0)
        interrupt_reactor();

    wakeup_event_.notify_all();
}

void
select_scheduler::
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
select_scheduler::
post(scheduler_op* h) const
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

void
select_scheduler::
on_work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
select_scheduler::
on_work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

bool
select_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
select_scheduler::
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
select_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
select_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
select_scheduler::
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
select_scheduler::
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
select_scheduler::
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
select_scheduler::
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
select_scheduler::
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
select_scheduler::
register_fd(int fd, select_op* op, int events) const
{
    // Validate fd is within select() limits
    if (fd < 0 || fd >= FD_SETSIZE)
        detail::throw_system_error(make_err(EINVAL), "select: fd out of range");

    {
        std::lock_guard lock(mutex_);

        auto& state = registered_fds_[fd];
        if (events & event_read)
            state.read_op = op;
        if (events & event_write)
            state.write_op = op;

        if (fd > max_fd_)
            max_fd_ = fd;
    }

    // Wake the reactor so a thread blocked in select() rebuilds its fd_sets
    // with the newly registered fd.
    interrupt_reactor();
}

void
select_scheduler::
deregister_fd(int fd, int events) const
{
    std::lock_guard lock(mutex_);

    auto it = registered_fds_.find(fd);
    if (it == registered_fds_.end())
        return;

    if (events & event_read)
        it->second.read_op = nullptr;
    if (events & event_write)
        it->second.write_op = nullptr;

    // Remove entry if both are null
    if (!it->second.read_op && !it->second.write_op)
    {
        registered_fds_.erase(it);

        // Recalculate max_fd_ if needed
        if (fd == max_fd_)
        {
            max_fd_ = pipe_fds_[0];  // At minimum, the pipe read end
            for (auto& [registered_fd, state] : registered_fds_)
            {
                if (registered_fd > max_fd_)
                    max_fd_ = registered_fd;
            }
        }
    }
}

void
select_scheduler::
work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
select_scheduler::
work_finished() const noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Last work item completed - wake all threads so they can exit.
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
select_scheduler::
interrupt_reactor() const
{
    char byte = 1;
    [[maybe_unused]] auto r = ::write(pipe_fds_[1], &byte, 1);
}

void
select_scheduler::
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
        // No idle workers but reactor is running - interrupt it
        reactor_interrupted_ = true;
        lock.unlock();
        interrupt_reactor();
    }
    else
    {
        // No one to wake
        lock.unlock();
    }
}

struct work_guard
{
    select_scheduler const* self;
    ~work_guard() { self->work_finished(); }
};

long
select_scheduler::
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
select_scheduler::
run_reactor(std::unique_lock<std::mutex>& lock)
{
    // Calculate timeout considering timers, use 0 if interrupted
    long effective_timeout_us = reactor_interrupted_ ? 0 : calculate_timeout(-1);

    // Build fd_sets from registered_fds_
    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    // Always include the interrupt pipe
    FD_SET(pipe_fds_[0], &read_fds);
    int nfds = pipe_fds_[0];

    // Add registered fds
    for (auto& [fd, state] : registered_fds_)
    {
        if (state.read_op)
            FD_SET(fd, &read_fds);
        if (state.write_op)
        {
            FD_SET(fd, &write_fds);
            // Also monitor for errors on connect operations
            FD_SET(fd, &except_fds);
        }
        if (fd > nfds)
            nfds = fd;
    }

    // Convert timeout to timeval
    struct timeval tv;
    struct timeval* tv_ptr = nullptr;
    if (effective_timeout_us >= 0)
    {
        tv.tv_sec = effective_timeout_us / 1000000;
        tv.tv_usec = effective_timeout_us % 1000000;
        tv_ptr = &tv;
    }

    lock.unlock();

    int ready = ::select(nfds + 1, &read_fds, &write_fds, &except_fds, tv_ptr);
    int saved_errno = errno;

    // Process timers outside the lock
    timer_svc_->process_expired();

    if (ready < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "select");

    // Re-acquire lock before modifying completed_ops_
    lock.lock();

    // Drain the interrupt pipe if readable
    if (ready > 0 && FD_ISSET(pipe_fds_[0], &read_fds))
    {
        char buf[256];
        while (::read(pipe_fds_[0], buf, sizeof(buf)) > 0) {}
    }

    // Process I/O completions
    int completions_queued = 0;
    if (ready > 0)
    {
        // Iterate over registered fds (copy keys to avoid iterator invalidation)
        std::vector<int> fds_to_check;
        fds_to_check.reserve(registered_fds_.size());
        for (auto& [fd, state] : registered_fds_)
            fds_to_check.push_back(fd);

        for (int fd : fds_to_check)
        {
            auto it = registered_fds_.find(fd);
            if (it == registered_fds_.end())
                continue;

            auto& state = it->second;

            // Check for errors (especially for connect operations)
            bool has_error = FD_ISSET(fd, &except_fds);

            // Process read readiness
            if (state.read_op && (FD_ISSET(fd, &read_fds) || has_error))
            {
                auto* op = state.read_op;
                // Claim the op by exchanging to unregistered. Both registering and
                // registered states mean the op is ours to complete.
                auto prev = op->registered.exchange(
                    select_registration_state::unregistered, std::memory_order_acq_rel);
                if (prev != select_registration_state::unregistered)
                {
                    state.read_op = nullptr;

                    if (has_error)
                    {
                        int errn = 0;
                        socklen_t len = sizeof(errn);
                        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
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
            }

            // Process write readiness
            if (state.write_op && (FD_ISSET(fd, &write_fds) || has_error))
            {
                auto* op = state.write_op;
                // Claim the op by exchanging to unregistered. Both registering and
                // registered states mean the op is ours to complete.
                auto prev = op->registered.exchange(
                    select_registration_state::unregistered, std::memory_order_acq_rel);
                if (prev != select_registration_state::unregistered)
                {
                    state.write_op = nullptr;

                    if (has_error)
                    {
                        int errn = 0;
                        socklen_t len = sizeof(errn);
                        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
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
            }

            // Clean up empty entries
            if (!state.read_op && !state.write_op)
                registered_fds_.erase(it);
        }
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
select_scheduler::
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

#endif // !defined(_WIN32)
