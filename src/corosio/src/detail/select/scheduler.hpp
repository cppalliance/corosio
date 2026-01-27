//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SELECT_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_SELECT_SCHEDULER_HPP


#if !defined(_WIN32)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include "src/detail/scheduler_op.hpp"
#include "src/detail/timer_service.hpp"

#include <sys/select.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
class select_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    /** Construct the scheduler.

        Creates a self-pipe for reactor interruption.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    select_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);

    ~select_scheduler();

    select_scheduler(select_scheduler const&) = delete;
    select_scheduler& operator=(select_scheduler const&) = delete;

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

    /** Return the maximum file descriptor value supported.

        Returns FD_SETSIZE - 1, the maximum fd value that can be
        monitored by select(). Operations with fd >= FD_SETSIZE
        will fail with EINVAL.

        @return The maximum supported file descriptor value.
    */
    static constexpr int max_fd() noexcept { return FD_SETSIZE - 1; }

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

    /** For use by I/O operations to track pending work. */
    void work_started() const noexcept override;

    /** For use by I/O operations to track completed work. */
    void work_finished() const noexcept override;

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
    int pipe_fds_[2];  // [0]=read, [1]=write

    mutable std::mutex mutex_;
    mutable std::condition_variable wakeup_event_;
    mutable op_queue completed_ops_;
    mutable std::atomic<long> outstanding_work_;
    std::atomic<bool> stopped_;
    bool shutdown_;
    timer_service* timer_svc_ = nullptr;

    // Per-fd state for tracking registered operations
    struct fd_state
    {
        select_op* read_op = nullptr;
        select_op* write_op = nullptr;
    };
    mutable std::unordered_map<int, fd_state> registered_fds_;
    mutable int max_fd_ = -1;

    // Single reactor thread coordination
    mutable bool reactor_running_ = false;
    mutable bool reactor_interrupted_ = false;
    mutable int idle_thread_count_ = 0;
};

} // namespace boost::corosio::detail

#endif // !defined(_WIN32)

#endif // BOOST_COROSIO_DETAIL_SELECT_SCHEDULER_HPP
