//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include "src/detail/scheduler_op.hpp"
#include "src/detail/timer_service.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace boost::corosio::detail {

struct kqueue_op;
struct descriptor_state;
struct scheduler_context;

/** macOS/BSD scheduler using kqueue for I/O multiplexing.

    This scheduler implements the scheduler interface using the BSD kqueue
    API for efficient I/O event notification. It uses a single reactor model
    where one thread runs kevent() while other threads
    wait on a condition variable for handler work. This design provides:

    - Handler parallelism: N posted handlers can execute on N threads
    - No thundering herd: condition_variable wakes exactly one thread
    - IOCP parity: Behavior matches Windows I/O completion port semantics

    When threads call run(), they first try to execute queued handlers.
    If the queue is empty and no reactor is running, one thread becomes
    the reactor and runs kevent(). Other threads wait on a condition
    variable until handlers are available.

    kqueue uses EV_CLEAR for edge-triggered semantics (equivalent to
    epoll's EPOLLET). File descriptors are registered once with both
    EVFILT_READ and EVFILT_WRITE and stay registered until closed.

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

        Creates a kqueue file descriptor via kqueue(), sets
        close-on-exec, and registers EVFILT_USER for reactor
        interruption. On failure the kqueue fd is closed before
        throwing.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).

        @throws std::system_error if kqueue() fails, if setting
            FD_CLOEXEC on the kqueue fd fails, or if registering
            the EVFILT_USER event fails. The error code contains
            the errno from the failed syscall.
    */
    kqueue_scheduler(
        capy::execution_context& ctx,
        int concurrency_hint = -1);

    /** Destructor.

        Closes the kqueue file descriptor if valid. Does not throw.
    */
    ~kqueue_scheduler();

    kqueue_scheduler(kqueue_scheduler const&) = delete;
    kqueue_scheduler& operator=(kqueue_scheduler const&) = delete;

    void shutdown() override;
    void post(std::coroutine_handle<> h) const override;
    void post(scheduler_op* h) const override;
    // scheduler::on_work_started / on_work_finished — non-const, for executors.
    // Tracks work that keeps run() alive; the scheduler stops when the
    // count drops to zero.
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
    int kq_fd() const noexcept { return kq_fd_; }

    /** Register a descriptor for persistent monitoring.

        Adds EVFILT_READ and EVFILT_WRITE (both EV_CLEAR) for @a fd
        and stores @a desc in the kevent udata field so that the
        reactor can dispatch events to the correct descriptor_state.

        The caller retains ownership of @a desc. It must remain valid
        until deregister_descriptor() is called and all pending
        read/write/connect operations referencing it have completed.
        The scheduler accesses @a desc asynchronously from the reactor
        thread when kevent delivers events.

        @param fd The file descriptor to register.
        @param desc Pointer to the caller-owned descriptor_state.

        @throws std::system_error if kevent(EV_ADD) fails.
    */
    void register_descriptor(int fd, descriptor_state* desc) const;

    /** Deregister a persistently registered descriptor.

        Issues kevent(EV_DELETE) for both EVFILT_READ and EVFILT_WRITE.
        Errors are silently ignored because the fd may already be
        closed and kqueue automatically removes closed descriptors.

        After this call returns, the reactor will not deliver any
        further events for @a fd, so the associated descriptor_state
        may be safely destroyed once all previously queued completions
        have been processed.

        @param fd The file descriptor to deregister.
    */
    void deregister_descriptor(int fd) const;

    // scheduler::work_started / work_finished — const, for I/O services.
    // Adjusts outstanding_work_ and wakes blocked threads but does not
    // stop the scheduler when the count reaches zero.
    void work_started() const noexcept override;
    void work_finished() const noexcept override;

    /** Offset a forthcoming work_finished from work_cleanup.

        Called by descriptor_state when all I/O returned EAGAIN and no
        handler will be executed. Must be called from a scheduler thread.
    */
    void compensating_work_started() const noexcept;

    /** Drain work from thread context's private queue to global queue.

        Called by thread_context_guard destructor when a thread exits run().
        Transfers pending work to the global queue under mutex protection.

        @param queue The private queue to drain.
        @param count Item count for wakeup decisions (wakes other threads if positive).
    */
    void drain_thread_queue(op_queue& queue, std::int64_t count) const;

    /** Post completed operations for deferred invocation.

        If called from a thread running this scheduler, operations go to
        the thread's private queue (fast path). Otherwise, operations are
        added to the global queue under mutex and a waiter is signaled.

        @par Preconditions
        work_started() must have been called for each operation.

        @param ops Queue of operations to post.
    */
    void post_deferred_completions(op_queue& ops) const;

private:
    friend struct work_cleanup;
    friend struct task_cleanup;

    std::size_t do_one(std::unique_lock<std::mutex>& lock, long timeout_us, scheduler_context* ctx);
    void run_task(std::unique_lock<std::mutex>& lock, scheduler_context* ctx);
    void wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const;
    void interrupt_reactor() const;
    long calculate_timeout(long requested_timeout_us) const;

    /** Set the signaled state and wake all waiting threads.

        @par Preconditions
        Mutex must be held.

        @param lock The held mutex lock.
    */
    void signal_all(std::unique_lock<std::mutex>& lock) const;

    /** Set the signaled state and wake one waiter if any exist.

        Only unlocks and signals if at least one thread is waiting.
        Use this when the caller needs to perform a fallback action
        (such as interrupting the reactor) when no waiters exist.

        @par Preconditions
        Mutex must be held.

        @param lock The held mutex lock.

        @return `true` if unlocked and signaled, `false` if lock still held.
    */
    bool maybe_unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const;

    /** Set the signaled state, unlock, and wake one waiter if any exist.

        Always unlocks the mutex. Use this when the caller will release
        the lock regardless of whether a waiter exists.

        @par Preconditions
        Mutex must be held.

        @param lock The held mutex lock.
    */
    void unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const;

    /** Clear the signaled state before waiting.

        @par Preconditions
        Mutex must be held.
    */
    void clear_signal() const;

    /** Block until the signaled state is set.

        Returns immediately if already signaled (fast-path). Otherwise
        increments the waiter count, waits on the condition variable,
        and decrements the waiter count upon waking.

        @par Preconditions
        Mutex must be held.

        @param lock The held mutex lock.
    */
    void wait_for_signal(std::unique_lock<std::mutex>& lock) const;

    /** Block until signaled or timeout expires.

        @par Preconditions
        Mutex must be held.

        @param lock The held mutex lock.
        @param timeout_us Maximum time to wait in microseconds.
    */
    void wait_for_signal_for(
        std::unique_lock<std::mutex>& lock,
        long timeout_us) const;

    int kq_fd_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cond_;
    mutable op_queue completed_ops_;
    mutable std::atomic<std::int64_t> outstanding_work_{0};
    std::atomic<bool> stopped_{false};
    bool shutdown_ = false;
    timer_service* timer_svc_ = nullptr;

    // True while a thread is blocked in kevent(). Used by
    // wake_one_thread_and_unlock and work_finished to know when
    // an EVFILT_USER interrupt is needed instead of a condvar signal.
    mutable bool task_running_ = false;

    // True when the reactor has been told to do a non-blocking poll
    // (more handlers queued or poll mode). Prevents redundant EVFILT_USER
    // triggers and controls the kevent() timeout.
    mutable bool task_interrupted_ = false;

    // Signaling state: bit 0 = signaled, upper bits = waiter count
    static constexpr std::size_t signaled_bit     = 1;
    static constexpr std::size_t waiter_increment = 2;
    mutable std::size_t state_ = 0;

    // EVFILT_USER idempotency: prevents redundant NOTE_TRIGGER writes
    mutable std::atomic<bool> user_event_armed_{false};

    // Sentinel operation for interleaving reactor runs with handler execution.
    // Ensures the reactor runs periodically even when handlers are continuously
    // posted, preventing starvation of I/O events, timers, and signals.
    struct task_op final : scheduler_op
    {
        void operator()() override {}
        void destroy() override {}
    };
    task_op task_op_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_SCHEDULER_HPP
