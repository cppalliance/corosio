//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>

#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#include <boost/corosio/native/detail/posix/posix_stream_file_service.hpp>
#include <boost/corosio/native/detail/posix/posix_random_access_file_service.hpp>

#include <boost/corosio/detail/except.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace boost::corosio::detail {

struct epoll_op;
struct descriptor_state;

/** Linux scheduler using epoll for I/O multiplexing.

    This scheduler implements the scheduler interface using Linux epoll
    for efficient I/O event notification. It uses a single reactor model
    where one thread runs epoll_wait while other threads
    wait on a condition variable for handler work. This design provides:

    - Handler parallelism: N posted handlers can execute on N threads
    - No thundering herd: condition_variable wakes exactly one thread
    - IOCP parity: Behavior matches Windows I/O completion port semantics

    When threads call run(), they first try to execute queued handlers.
    If the queue is empty and no reactor is running, one thread becomes
    the reactor and runs epoll_wait. Other threads wait on a condition
    variable until handlers are available.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL epoll_scheduler final : public reactor_scheduler_base
{
public:
    /** Construct the scheduler.

        Creates an epoll instance, eventfd for reactor interruption,
        and timerfd for kernel-managed timer expiry.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    epoll_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);

    /// Destroy the scheduler.
    ~epoll_scheduler() override;

    epoll_scheduler(epoll_scheduler const&)            = delete;
    epoll_scheduler& operator=(epoll_scheduler const&) = delete;

    /// Shut down the scheduler, draining pending operations.
    void shutdown() override;

    /** Return the epoll file descriptor.

        Used by socket services to register file descriptors
        for I/O event notification.

        @return The epoll file descriptor.
    */
    int epoll_fd() const noexcept
    {
        return epoll_fd_;
    }

    /** Register a descriptor for persistent monitoring.

        The fd is registered once and stays registered until explicitly
        deregistered. Events are dispatched via descriptor_state which
        tracks pending read/write/connect operations.

        @param fd The file descriptor to register.
        @param desc Pointer to descriptor data (stored in epoll_event.data.ptr).
    */
    void register_descriptor(int fd, descriptor_state* desc) const;

    /** Deregister a persistently registered descriptor.

        @param fd The file descriptor to deregister.
    */
    void deregister_descriptor(int fd) const;

private:
    void
    run_task(std::unique_lock<std::mutex>& lock, context_type* ctx) override;
    void interrupt_reactor() const override;
    void update_timerfd() const;

    int epoll_fd_;
    int event_fd_;
    int timer_fd_;

    // Edge-triggered eventfd state
    mutable std::atomic<bool> eventfd_armed_{false};

    // Set when the earliest timer changes; flushed before epoll_wait
    mutable std::atomic<bool> timerfd_stale_{false};
};

inline epoll_scheduler::epoll_scheduler(capy::execution_context& ctx, int)
    : epoll_fd_(-1)
    , event_fd_(-1)
    , timer_fd_(-1)
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
    ev.events   = EPOLLIN | EPOLLET;
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
    timer_ev.events   = EPOLLIN | EPOLLERR;
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
        timer_service::callback(this, [](void* p) {
            auto* self = static_cast<epoll_scheduler*>(p);
            self->timerfd_stale_.store(true, std::memory_order_release);
            self->interrupt_reactor();
        }));

    get_resolver_service(ctx, *this);
    get_signal_service(ctx, *this);
    get_stream_file_service(ctx, *this);
    get_random_access_file_service(ctx, *this);

    completed_ops_.push(&task_op_);
}

inline epoll_scheduler::~epoll_scheduler()
{
    if (timer_fd_ >= 0)
        ::close(timer_fd_);
    if (event_fd_ >= 0)
        ::close(event_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

inline void
epoll_scheduler::shutdown()
{
    shutdown_drain();

    if (event_fd_ >= 0)
        interrupt_reactor();
}

inline void
epoll_scheduler::register_descriptor(int fd, descriptor_state* desc) const
{
    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLERR | EPOLLHUP;
    ev.data.ptr = desc;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        detail::throw_system_error(make_err(errno), "epoll_ctl (register)");

    desc->registered_events = ev.events;
    desc->fd                = fd;
    desc->scheduler_        = this;
    desc->ready_events_.store(0, std::memory_order_relaxed);

    std::lock_guard lock(desc->mutex);
    desc->impl_ref_.reset();
    desc->read_ready  = false;
    desc->write_ready = false;
}

inline void
epoll_scheduler::deregister_descriptor(int fd) const
{
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

inline void
epoll_scheduler::interrupt_reactor() const
{
    bool expected = false;
    if (eventfd_armed_.compare_exchange_strong(
            expected, true, std::memory_order_release,
            std::memory_order_relaxed))
    {
        std::uint64_t val       = 1;
        [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
    }
}

inline void
epoll_scheduler::update_timerfd() const
{
    auto nearest = timer_svc_->nearest_expiry();

    itimerspec ts{};
    int flags = 0;

    if (nearest == timer_service::time_point::max())
    {
        // No timers — disarm by setting to 0 (relative)
    }
    else
    {
        auto now = std::chrono::steady_clock::now();
        if (nearest <= now)
        {
            // Use 1ns instead of 0 — zero disarms the timerfd
            ts.it_value.tv_nsec = 1;
        }
        else
        {
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            nearest - now)
                            .count();
            ts.it_value.tv_sec  = nsec / 1000000000;
            ts.it_value.tv_nsec = nsec % 1000000000;
            if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
                ts.it_value.tv_nsec = 1;
        }
    }

    if (::timerfd_settime(timer_fd_, flags, &ts, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "timerfd_settime");
}

inline void
epoll_scheduler::run_task(std::unique_lock<std::mutex>& lock, context_type* ctx)
{
    int timeout_ms = task_interrupted_ ? 0 : -1;

    if (lock.owns_lock())
        lock.unlock();

    task_cleanup on_exit{this, &lock, ctx};

    // Flush deferred timerfd programming before blocking
    if (timerfd_stale_.exchange(false, std::memory_order_acquire))
        update_timerfd();

    epoll_event events[128];
    int nfds = ::epoll_wait(epoll_fd_, events, 128, timeout_ms);

    if (nfds < 0 && errno != EINTR)
        detail::throw_system_error(make_err(errno), "epoll_wait");

    bool check_timers = false;
    op_queue local_ops;

    for (int i = 0; i < nfds; ++i)
    {
        if (events[i].data.ptr == nullptr)
        {
            std::uint64_t val;
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
            eventfd_armed_.store(false, std::memory_order_relaxed);
            continue;
        }

        if (events[i].data.ptr == &timer_fd_)
        {
            std::uint64_t expirations;
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            [[maybe_unused]] auto r =
                ::read(timer_fd_, &expirations, sizeof(expirations));
            check_timers = true;
            continue;
        }

        auto* desc = static_cast<descriptor_state*>(events[i].data.ptr);
        desc->add_ready_events(events[i].events);

        bool expected = false;
        if (desc->is_enqueued_.compare_exchange_strong(
                expected, true, std::memory_order_release,
                std::memory_order_relaxed))
        {
            local_ops.push(desc);
        }
    }

    if (check_timers)
    {
        timer_svc_->process_expired();
        update_timerfd();
    }

    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP
