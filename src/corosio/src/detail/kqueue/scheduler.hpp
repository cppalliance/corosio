//
// Copyright (c) 2026 Cinar Gursoy
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include "src/detail/scheduler_op.hpp"
#include "src/detail/timer_service.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace boost {
namespace corosio {
namespace detail {

struct kqueue_op;

/** macOS/BSD scheduler using kqueue for I/O multiplexing.

    This scheduler implements the scheduler interface using BSD kqueue
    for efficient I/O event notification. It manages a queue of handlers
    and provides blocking/non-blocking execution methods.

    The scheduler uses EVFILT_USER to wake up kevent when non-I/O
    handlers are posted, enabling efficient integration of both
    I/O completions and posted handlers.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class kqueue_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type = scheduler;

    /** Construct the scheduler.

        Creates a kqueue instance and registers EVFILT_USER for wakeup.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    kqueue_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);

    ~kqueue_scheduler();

    kqueue_scheduler(kqueue_scheduler const&) = delete;
    kqueue_scheduler& operator=(kqueue_scheduler const&) = delete;

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

    /** Return the kqueue file descriptor.

        Used by socket services to register file descriptors
        for I/O event notification.

        @return The kqueue file descriptor.
    */
    int kqueue_fd() const noexcept { return kqueue_fd_; }

    /** Register a file descriptor with kqueue.

        @param fd The file descriptor to register.
        @param op The operation associated with this fd.
        @param filter The kqueue filter (EVFILT_READ, EVFILT_WRITE).
    */
    void register_fd(int fd, kqueue_op* op, int16_t filter) const;

    /** Modify kqueue registration for a file descriptor.

        @param fd The file descriptor to modify.
        @param op The operation associated with this fd.
        @param filter The new kqueue filter.
    */
    void modify_fd(int fd, kqueue_op* op, int16_t filter) const;

    /** Unregister a file descriptor from kqueue.

        @param fd The file descriptor to unregister.
        @param filter The filter to remove.
    */
    void unregister_fd(int fd, int16_t filter) const;

    /** For use by I/O operations to track pending work. */
    void work_started() const noexcept;

    /** For use by I/O operations to track completed work. */
    void work_finished() const noexcept;

private:
    std::size_t do_one(long timeout_us);
    void wakeup() const;
    long calculate_timeout(long requested_timeout_us) const;

    int kqueue_fd_;
    mutable std::mutex mutex_;
    mutable op_queue completed_ops_;
    mutable std::atomic<long> outstanding_work_;
    std::atomic<bool> stopped_;
    bool shutdown_;
    timer_service* timer_svc_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_BACKEND_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP
