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

#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>

#include <boost/corosio/native/detail/select/select_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>

#include <boost/corosio/detail/except.hpp>

#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace boost::corosio::detail {

struct select_op;
struct select_descriptor_state;

/** POSIX scheduler using select() for I/O multiplexing.

    This scheduler implements the scheduler interface using the POSIX select()
    call for I/O event notification. It inherits the shared reactor threading
    model from reactor_scheduler_base: signal state machine, inline completion
    budget, work counting, and the do_one event loop.

    The design mirrors epoll_scheduler for behavioral consistency:
    - Same single-reactor thread coordination model
    - Same deferred I/O pattern (reactor marks ready; workers do I/O)
    - Same timer integration pattern

    Known Limitations:
    - FD_SETSIZE (~1024) limits maximum concurrent connections
    - O(n) scanning: rebuilds fd_sets each iteration
    - Level-triggered only (no edge-triggered mode)

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL select_scheduler final : public reactor_scheduler_base
{
public:
    /** Construct the scheduler.

        Creates a self-pipe for reactor interruption.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    select_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);

    /// Destroy the scheduler.
    ~select_scheduler() override;

    select_scheduler(select_scheduler const&)            = delete;
    select_scheduler& operator=(select_scheduler const&) = delete;

    /// Shut down the scheduler, draining pending operations.
    void shutdown() override;

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

    /** Register a descriptor for persistent monitoring.

        The fd is added to the registered_descs_ map and will be
        included in subsequent select() calls. The reactor is
        interrupted so a blocked select() rebuilds its fd_sets.

        @param fd The file descriptor to register.
        @param desc Pointer to descriptor state for this fd.
    */
    void register_descriptor(int fd, select_descriptor_state* desc) const;

    /** Deregister a persistently registered descriptor.

        @param fd The file descriptor to deregister.
    */
    void deregister_descriptor(int fd) const;

    /** Interrupt the reactor so it rebuilds its fd_sets.

        Called when a write or connect op is registered after
        the reactor's snapshot was taken. Without this, select()
        may block not watching for writability on the fd.
    */
    void notify_reactor() const;

private:
    void
    run_task(std::unique_lock<std::mutex>& lock, context_type* ctx) override;
    void interrupt_reactor() const override;
    long calculate_timeout(long requested_timeout_us) const;

    // Self-pipe for interrupting select()
    int pipe_fds_[2]; // [0]=read, [1]=write

    // Per-fd tracking for fd_set building
    mutable std::unordered_map<int, select_descriptor_state*> registered_descs_;
    mutable int max_fd_ = -1;
};

inline select_scheduler::select_scheduler(capy::execution_context& ctx, int)
    : pipe_fds_{-1, -1}
    , max_fd_(-1)
{
    if (::pipe(pipe_fds_) < 0)
        detail::throw_system_error(make_err(errno), "pipe");

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

    get_resolver_service(ctx, *this);
    get_signal_service(ctx, *this);

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
    shutdown_drain();

    if (pipe_fds_[1] >= 0)
        interrupt_reactor();
}

inline void
select_scheduler::register_descriptor(
    int fd, select_descriptor_state* desc) const
{
    if (fd < 0 || fd >= FD_SETSIZE)
        detail::throw_system_error(make_err(EINVAL), "select: fd out of range");

    desc->registered_events = reactor_event_read | reactor_event_write;
    desc->fd                = fd;
    desc->scheduler_        = this;
    desc->ready_events_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard lock(desc->mutex);
        desc->impl_ref_.reset();
        desc->read_ready  = false;
        desc->write_ready = false;
    }

    {
        std::lock_guard lock(mutex_);
        registered_descs_[fd] = desc;
        if (fd > max_fd_)
            max_fd_ = fd;
    }

    interrupt_reactor();
}

inline void
select_scheduler::deregister_descriptor(int fd) const
{
    std::lock_guard lock(mutex_);

    auto it = registered_descs_.find(fd);
    if (it == registered_descs_.end())
        return;

    registered_descs_.erase(it);

    if (fd == max_fd_)
    {
        max_fd_ = pipe_fds_[0];
        for (auto& [registered_fd, state] : registered_descs_)
        {
            if (registered_fd > max_fd_)
                max_fd_ = registered_fd;
        }
    }
}

inline void
select_scheduler::notify_reactor() const
{
    interrupt_reactor();
}

inline void
select_scheduler::interrupt_reactor() const
{
    char byte               = 1;
    [[maybe_unused]] auto r = ::write(pipe_fds_[1], &byte, 1);
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

    constexpr auto long_max =
        static_cast<long long>((std::numeric_limits<long>::max)());
    auto capped_timer_us =
        (std::min)((std::max)(static_cast<long long>(timer_timeout_us),
                              static_cast<long long>(0)),
                   long_max);

    if (requested_timeout_us < 0)
        return static_cast<long>(capped_timer_us);

    return static_cast<long>(
        (std::min)(static_cast<long long>(requested_timeout_us),
                   capped_timer_us));
}

inline void
select_scheduler::run_task(
    std::unique_lock<std::mutex>& lock, context_type* ctx)
{
    long effective_timeout_us = task_interrupted_ ? 0 : calculate_timeout(-1);

    // Snapshot registered descriptors while holding lock.
    // Record which fds need write monitoring to avoid a hot loop:
    // select is level-triggered so writable sockets (nearly always
    // writable) would cause select() to return immediately every
    // iteration if unconditionally added to write_fds.
    struct fd_entry
    {
        int fd;
        select_descriptor_state* desc;
        bool needs_write;
    };
    fd_entry snapshot[FD_SETSIZE];
    int snapshot_count = 0;

    for (auto& [fd, desc] : registered_descs_)
    {
        if (snapshot_count < FD_SETSIZE)
        {
            std::lock_guard desc_lock(desc->mutex);
            snapshot[snapshot_count].fd   = fd;
            snapshot[snapshot_count].desc = desc;
            snapshot[snapshot_count].needs_write =
                (desc->write_op || desc->connect_op);
            ++snapshot_count;
        }
    }

    if (lock.owns_lock())
        lock.unlock();

    task_cleanup on_exit{this, &lock, ctx};

    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    FD_SET(pipe_fds_[0], &read_fds);
    int nfds = pipe_fds_[0];

    for (int i = 0; i < snapshot_count; ++i)
    {
        int fd = snapshot[i].fd;
        FD_SET(fd, &read_fds);
        if (snapshot[i].needs_write)
            FD_SET(fd, &write_fds);
        FD_SET(fd, &except_fds);
        if (fd > nfds)
            nfds = fd;
    }

    struct timeval tv;
    struct timeval* tv_ptr = nullptr;
    if (effective_timeout_us >= 0)
    {
        tv.tv_sec  = effective_timeout_us / 1000000;
        tv.tv_usec = effective_timeout_us % 1000000;
        tv_ptr     = &tv;
    }

    int ready = ::select(nfds + 1, &read_fds, &write_fds, &except_fds, tv_ptr);

    // EINTR: signal interrupted select(), just retry.
    // EBADF: an fd was closed between snapshot and select(); retry
    // with a fresh snapshot from registered_descs_.
    if (ready < 0)
    {
        if (errno == EINTR || errno == EBADF)
            return;
        detail::throw_system_error(make_err(errno), "select");
    }

    // Process timers outside the lock
    timer_svc_->process_expired();

    op_queue local_ops;

    if (ready > 0)
    {
        if (FD_ISSET(pipe_fds_[0], &read_fds))
        {
            char buf[256];
            while (::read(pipe_fds_[0], buf, sizeof(buf)) > 0)
            {
            }
        }

        for (int i = 0; i < snapshot_count; ++i)
        {
            int fd                        = snapshot[i].fd;
            select_descriptor_state* desc = snapshot[i].desc;

            std::uint32_t flags = 0;
            if (FD_ISSET(fd, &read_fds))
                flags |= reactor_event_read;
            if (FD_ISSET(fd, &write_fds))
                flags |= reactor_event_write;
            if (FD_ISSET(fd, &except_fds))
                flags |= reactor_event_error;

            if (flags == 0)
                continue;

            desc->add_ready_events(flags);

            bool expected = false;
            if (desc->is_enqueued_.compare_exchange_strong(
                    expected, true, std::memory_order_release,
                    std::memory_order_relaxed))
            {
                local_ops.push(desc);
            }
        }
    }

    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_SCHEDULER_HPP
