//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/native_scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>

#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

struct select_op;

/** POSIX scheduler using select() for I/O multiplexing.

    This scheduler implements the scheduler interface using the POSIX select()
    call for I/O event notification. It uses a single reactor model
    where one thread runs select() while other threads wait on a condition
    variable for handler work. This design provides:

    - Handler parallelism: N posted handlers can execute on N threads
    - No thundering herd: condition_variable wakes exactly one thread
    - Portability: Works on all POSIX systems

    The design mirrors epoll_scheduler for behavioral consistency:
    - Same single-reactor thread coordination model
    - Same work counting semantics
    - Same timer integration pattern

    Known Limitations:
    - FD_SETSIZE (~1024) limits maximum concurrent connections
    - O(n) scanning: rebuilds fd_sets each iteration
    - Level-triggered only (no edge-triggered mode)

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL select_scheduler final
    : public native_scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    /** Construct the scheduler.

        Creates a self-pipe for reactor interruption.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    select_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);

    ~select_scheduler() override;

    select_scheduler(select_scheduler const&)            = delete;
    select_scheduler& operator=(select_scheduler const&) = delete;

    void shutdown() override;
    void post(std::coroutine_handle<> h) const override;
    void post(scheduler_op* h) const override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

    /** Return the maximum file descriptor value supported.

        Returns FD_SETSIZE - 1, the maximum fd value that can be
        monitored by select(). Operations with fd >= FD_SETSIZE
        will fail with EINVAL.

        @return The maximum supported file descriptor value.
    */
    static constexpr int max_fd() noexcept
    {
        return FD_SETSIZE - 1;
    }

    /** Register a file descriptor for monitoring.

        @param fd The file descriptor to register.
        @param op The operation associated with this fd.
        @param events Event mask: 1 = read, 2 = write, 3 = both.
    */
    void register_fd(int fd, select_op* op, int events) const;

    /** Unregister a file descriptor from monitoring.

        @param fd The file descriptor to unregister.
        @param events Event mask to remove: 1 = read, 2 = write, 3 = both.
    */
    void deregister_fd(int fd, int events) const;

    void work_started() noexcept override;
    void work_finished() noexcept override;

    // Event flags for register_fd/deregister_fd
    static constexpr int event_read  = 1;
    static constexpr int event_write = 2;

private:
    std::size_t do_one(long timeout_us);
    void run_reactor(std::unique_lock<std::mutex>& lock);
    void wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const;
    void interrupt_reactor() const;
    long calculate_timeout(long requested_timeout_us) const;

    // Self-pipe for interrupting select()
    int pipe_fds_[2]; // [0]=read, [1]=write

    mutable std::mutex mutex_;
    mutable std::condition_variable wakeup_event_;
    mutable op_queue completed_ops_;
    mutable std::atomic<long> outstanding_work_;
    std::atomic<bool> stopped_;

    // Per-fd state for tracking registered operations
    struct fd_state
    {
        select_op* read_op  = nullptr;
        select_op* write_op = nullptr;
    };
    mutable std::unordered_map<int, fd_state> registered_fds_;
    mutable int max_fd_ = -1;

    // Single reactor thread coordination
    mutable bool reactor_running_     = false;
    mutable bool reactor_interrupted_ = false;
    mutable int idle_thread_count_    = 0;

    // Sentinel operation for interleaving reactor runs with handler execution.
    // Ensures the reactor runs periodically even when handlers are continuously
    // posted, preventing timer starvation.
    struct task_op final : scheduler_op
    {
        void operator()() override {}
        void destroy() override {}
    };
    task_op task_op_;
};

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

namespace select {

struct BOOST_COROSIO_SYMBOL_VISIBLE scheduler_context
{
    select_scheduler const* key;
    scheduler_context* next;
};

inline thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(select_scheduler const* ctx) noexcept
        : frame_{ctx, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        context_stack.set(frame_.next);
    }
};

struct work_guard
{
    select_scheduler* self;
    ~work_guard()
    {
        self->work_finished();
    }
};

} // namespace select

inline select_scheduler::select_scheduler(capy::execution_context& ctx, int)
    : pipe_fds_{-1, -1}
    , outstanding_work_(0)
    , stopped_(false)
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
        timer_service::callback(this, [](void* p) {
            static_cast<select_scheduler*>(p)->interrupt_reactor();
        }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);

    // Push task sentinel to interleave reactor runs with handler execution
    completed_ops_.push(&task_op_);
}

inline select_scheduler::~select_scheduler()
{
    if (pipe_fds_[0] >= 0)
        ::close(pipe_fds_[0]);
    if (pipe_fds_[1] >= 0)
        ::close(pipe_fds_[1]);
}

inline void
select_scheduler::shutdown()
{
    {
        std::unique_lock lock(mutex_);

        while (auto* h = completed_ops_.pop())
        {
            if (h == &task_op_)
                continue;
            lock.unlock();
            h->destroy();
            lock.lock();
        }
    }

    if (pipe_fds_[1] >= 0)
        interrupt_reactor();

    wakeup_event_.notify_all();
}

inline void
select_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;

        explicit post_handler(std::coroutine_handle<> h) : h_(h) {}

        ~post_handler() override = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
            h.resume();
        }

        void destroy() override
        {
            auto h = h_;
            delete this;
            h.destroy();
        }
    };

    auto ph = std::make_unique<post_handler>(h);
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(ph.release());
    wake_one_thread_and_unlock(lock);
}

inline void
select_scheduler::post(scheduler_op* h) const
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

inline bool
select_scheduler::running_in_this_thread() const noexcept
{
    for (auto* c = select::context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

inline void
select_scheduler::stop()
{
    bool expected = false;
    if (stopped_.compare_exchange_strong(
            expected, true, std::memory_order_release,
            std::memory_order_relaxed))
    {
        // Wake all threads so they notice stopped_ and exit
        {
            std::lock_guard lock(mutex_);
            wakeup_event_.notify_all();
        }
        interrupt_reactor();
    }
}

inline bool
select_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

inline void
select_scheduler::restart()
{
    stopped_.store(false, std::memory_order_release);
}

inline std::size_t
select_scheduler::run()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    select::thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(-1))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

inline std::size_t
select_scheduler::run_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    select::thread_context_guard ctx(this);
    return do_one(-1);
}

inline std::size_t
select_scheduler::wait_one(long usec)
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    select::thread_context_guard ctx(this);
    return do_one(usec);
}

inline std::size_t
select_scheduler::poll()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    select::thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(0))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

inline std::size_t
select_scheduler::poll_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    select::thread_context_guard ctx(this);
    return do_one(0);
}

inline void
select_scheduler::register_fd(int fd, select_op* op, int events) const
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

inline void
select_scheduler::deregister_fd(int fd, int events) const
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
            max_fd_ = pipe_fds_[0]; // At minimum, the pipe read end
            for (auto& [registered_fd, state] : registered_fds_)
            {
                if (registered_fd > max_fd_)
                    max_fd_ = registered_fd;
            }
        }
    }
}

inline void
select_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
select_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

inline void
select_scheduler::interrupt_reactor() const
{
    char byte               = 1;
    [[maybe_unused]] auto r = ::write(pipe_fds_[1], &byte, 1);
}

inline void
select_scheduler::wake_one_thread_and_unlock(
    std::unique_lock<std::mutex>& lock) const
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

inline long
select_scheduler::calculate_timeout(long requested_timeout_us) const
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
    auto capped_timer_us =
        (std::min)((std::max)(static_cast<long long>(timer_timeout_us),
                              static_cast<long long>(0)),
                   long_max);

    if (requested_timeout_us < 0)
        return static_cast<long>(capped_timer_us);

    // requested_timeout_us is already long, so min() result fits in long
    return static_cast<long>(
        (std::min)(static_cast<long long>(requested_timeout_us),
                   capped_timer_us));
}

inline void
select_scheduler::run_reactor(std::unique_lock<std::mutex>& lock)
{
    // Calculate timeout considering timers, use 0 if interrupted
    long effective_timeout_us =
        reactor_interrupted_ ? 0 : calculate_timeout(-1);

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
        tv.tv_sec  = effective_timeout_us / 1000000;
        tv.tv_usec = effective_timeout_us % 1000000;
        tv_ptr     = &tv;
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
        while (::read(pipe_fds_[0], buf, sizeof(buf)) > 0)
        {
        }
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
                    select_registration_state::unregistered,
                    std::memory_order_acq_rel);
                if (prev != select_registration_state::unregistered)
                {
                    state.read_op = nullptr;

                    if (has_error)
                    {
                        int errn      = 0;
                        socklen_t len = sizeof(errn);
                        if (::getsockopt(
                                fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
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
                    select_registration_state::unregistered,
                    std::memory_order_acq_rel);
                if (prev != select_registration_state::unregistered)
                {
                    state.write_op = nullptr;

                    if (has_error)
                    {
                        int errn      = 0;
                        socklen_t len = sizeof(errn);
                        if (::getsockopt(
                                fd, SOL_SOCKET, SO_ERROR, &errn, &len) < 0)
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

    if (completions_queued > 0)
    {
        if (completions_queued == 1)
            wakeup_event_.notify_one();
        else
            wakeup_event_.notify_all();
    }
}

inline std::size_t
select_scheduler::do_one(long timeout_us)
{
    std::unique_lock lock(mutex_);

    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        scheduler_op* op = completed_ops_.pop();

        if (op == &task_op_)
        {
            bool more_handlers = !completed_ops_.empty();

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
            reactor_running_     = true;

            if (more_handlers && idle_thread_count_ > 0)
                wakeup_event_.notify_one();

            run_reactor(lock);

            reactor_running_ = false;
            completed_ops_.push(&task_op_);
            continue;
        }

        if (op != nullptr)
        {
            lock.unlock();
            select::work_guard g{this};
            (*op)();
            return 1;
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

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SCHEDULER_HPP
