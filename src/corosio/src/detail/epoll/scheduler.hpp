//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_SCHEDULER_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_EPOLL)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include "src/detail/scheduler_op.hpp"
#include "src/detail/timer_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace boost::corosio::detail {

struct epoll_op;

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
class epoll_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    /** Construct the scheduler.

        Creates an epoll instance and eventfd for event notification.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    epoll_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);

    ~epoll_scheduler();

    epoll_scheduler(epoll_scheduler const&) = delete;
    epoll_scheduler& operator=(epoll_scheduler const&) = delete;

    void shutdown() override;
    void post(capy::coro h) const override;
    void post(scheduler_op* h) const override;
    void on_work_started() noexcept override;
    void on_work_finished() noexcept override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

    /** Return the epoll file descriptor.

        Used by socket services to register file descriptors
        for I/O event notification.

        @return The epoll file descriptor.
    */
    int epoll_fd() const noexcept { return epoll_fd_; }

    /** Register a file descriptor with epoll.

        @param fd The file descriptor to register.
        @param op The operation associated with this fd.
        @param events The epoll events to monitor (EPOLLIN, EPOLLOUT, etc.).
    */
    void register_fd(int fd, epoll_op* op, std::uint32_t events) const;

    /** Modify epoll registration for a file descriptor.

        @param fd The file descriptor to modify.
        @param op The operation associated with this fd.
        @param events The new epoll events to monitor.
    */
    void modify_fd(int fd, epoll_op* op, std::uint32_t events) const;

    /** Unregister a file descriptor from epoll.

        @param fd The file descriptor to unregister.
    */
    void unregister_fd(int fd) const;

    /** For use by I/O operations to track pending work. */
    void work_started() const noexcept override;

    /** For use by I/O operations to track completed work. */
    void work_finished() const noexcept override;

private:
    std::size_t do_one(long timeout_us);
    void run_reactor(std::unique_lock<std::mutex>& lock);
    void wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const;
    void interrupt_reactor() const;
    long calculate_timeout(long requested_timeout_us) const;

    int epoll_fd_;
    int event_fd_;                              // for interrupting reactor
    mutable std::mutex mutex_;
    mutable std::condition_variable wakeup_event_;
    mutable op_queue completed_ops_;
    mutable std::atomic<long> outstanding_work_;
    std::atomic<bool> stopped_;
    bool shutdown_;
    timer_service* timer_svc_ = nullptr;

    // Single reactor thread coordination
    mutable bool reactor_running_ = false;
    mutable bool reactor_interrupted_ = false;
    mutable int idle_thread_count_ = 0;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_BACKEND_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_SCHEDULER_HPP
