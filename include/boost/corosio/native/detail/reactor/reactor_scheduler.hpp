//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SCHEDULER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>
#include <boost/corosio/detail/conditionally_enabled_event.hpp>

namespace boost::corosio::detail {

// Forward declarations
class reactor_scheduler;
class timer_service;

/** Per-thread state for a reactor scheduler.

    Each thread running a scheduler's event loop has one of these
    on a thread-local stack. It holds a private work queue and
    inline completion budget for speculative I/O fast paths.
*/
struct BOOST_COROSIO_SYMBOL_VISIBLE reactor_scheduler_context
{
    /// Scheduler this context belongs to.
    reactor_scheduler const* key;

    /// Next context frame on this thread's stack.
    reactor_scheduler_context* next;

    /// Private work queue for reduced contention.
    op_queue private_queue;

    /// Unflushed work count for the private queue.
    std::int64_t private_outstanding_work;

    /// Remaining inline completions allowed this cycle.
    int inline_budget;

    /// Maximum inline budget (adaptive, 2-16).
    int inline_budget_max;

    /// True if no other thread absorbed queued work last cycle.
    bool unassisted;

    /// Construct a context frame linked to @a n.
    reactor_scheduler_context(
        reactor_scheduler const* k,
        reactor_scheduler_context* n);
};

/// Thread-local context stack for reactor schedulers.
inline thread_local_ptr<reactor_scheduler_context> reactor_context_stack;

/// Find the context frame for a scheduler on this thread.
inline reactor_scheduler_context*
reactor_find_context(reactor_scheduler const* self) noexcept
{
    for (auto* c = reactor_context_stack.get(); c != nullptr; c = c->next)
    {
        if (c->key == self)
            return c;
    }
    return nullptr;
}

/// Flush private work count to global counter.
inline void
reactor_flush_private_work(
    reactor_scheduler_context* ctx,
    std::atomic<std::int64_t>& outstanding_work) noexcept
{
    if (ctx && ctx->private_outstanding_work > 0)
    {
        outstanding_work.fetch_add(
            ctx->private_outstanding_work, std::memory_order_relaxed);
        ctx->private_outstanding_work = 0;
    }
}

/** Drain private queue to global queue, flushing work count first.

    @return True if any ops were drained.
*/
inline bool
reactor_drain_private_queue(
    reactor_scheduler_context* ctx,
    std::atomic<std::int64_t>& outstanding_work,
    op_queue& completed_ops) noexcept
{
    if (!ctx || ctx->private_queue.empty())
        return false;

    reactor_flush_private_work(ctx, outstanding_work);
    completed_ops.splice(ctx->private_queue);
    return true;
}

/** Non-template base for reactor-backed scheduler implementations.

    Provides the complete threading model shared by epoll, kqueue,
    and select schedulers: signal state machine, inline completion
    budget, work counting, run/poll methods, and the do_one event
    loop.

    Derived classes provide platform-specific hooks by overriding:
    - `run_task(lock, ctx)` to run the reactor poll
    - `interrupt_reactor()` to wake a blocked reactor

    De-templated from the original CRTP design to eliminate
    duplicate instantiations when multiple backends are compiled
    into the same binary. Virtual dispatch for run_task (called
    once per reactor cycle, before a blocking syscall) has
    negligible overhead.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class reactor_scheduler
    : public scheduler
    , public capy::execution_context::service
{
public:
    using key_type     = scheduler;
    using context_type = reactor_scheduler_context;
    using mutex_type = conditionally_enabled_mutex;
    using lock_type = mutex_type::scoped_lock;
    using event_type = conditionally_enabled_event;

    /// Post a coroutine for deferred execution.
    void post(std::coroutine_handle<> h) const override;

    /// Post a scheduler operation for deferred execution.
    void post(scheduler_op* h) const override;

    /// Return true if called from a thread running this scheduler.
    bool running_in_this_thread() const noexcept override;

    /// Request the scheduler to stop dispatching handlers.
    void stop() override;

    /// Return true if the scheduler has been stopped.
    bool stopped() const noexcept override;

    /// Reset the stopped state so `run()` can resume.
    void restart() override;

    /// Run the event loop until no work remains.
    std::size_t run() override;

    /// Run until one handler completes or no work remains.
    std::size_t run_one() override;

    /// Run until one handler completes or @a usec elapses.
    std::size_t wait_one(long usec) override;

    /// Run ready handlers without blocking.
    std::size_t poll() override;

    /// Run at most one ready handler without blocking.
    std::size_t poll_one() override;

    /// Increment the outstanding work count.
    void work_started() noexcept override;

    /// Decrement the outstanding work count, stopping on zero.
    void work_finished() noexcept override;

    /** Reset the thread's inline completion budget.

        Called at the start of each posted completion handler to
        grant a fresh budget for speculative inline completions.
    */
    void reset_inline_budget() const noexcept;

    /** Consume one unit of inline budget if available.

        @return True if budget was available and consumed.
    */
    bool try_consume_inline_budget() const noexcept;

    /** Offset a forthcoming work_finished from work_cleanup.

        Called by descriptor_state when all I/O returned EAGAIN and
        no handler will be executed. Must be called from a scheduler
        thread.
    */
    void compensating_work_started() const noexcept;

    /** Drain work from thread context's private queue to global queue.

        Flushes private work count to the global counter, then
        transfers the queue under mutex protection.

        @param queue The private queue to drain.
        @param count Private work count to flush before draining.
    */
    void drain_thread_queue(op_queue& queue, std::int64_t count) const;

    /** Post completed operations for deferred invocation.

        If called from a thread running this scheduler, operations
        go to the thread's private queue (fast path). Otherwise,
        operations are added to the global queue under mutex and a
        waiter is signaled.

        @par Preconditions
        work_started() must have been called for each operation.

        @param ops Queue of operations to post.
    */
    void post_deferred_completions(op_queue& ops) const;

    /** Apply runtime configuration to the scheduler.

        Called by `io_context` after construction. Values that do
        not apply to this backend are silently ignored.

        @param max_events  Event buffer size for epoll/kqueue.
        @param budget_init Starting inline completion budget.
        @param budget_max  Hard ceiling on adaptive budget ramp-up.
        @param unassisted  Budget when single-threaded.
    */
    virtual void configure_reactor(
        unsigned max_events,
        unsigned budget_init,
        unsigned budget_max,
        unsigned unassisted);

    /// Return the configured initial inline budget.
    unsigned inline_budget_initial() const noexcept
    {
        return inline_budget_initial_;
    }

    /// Return true if single-threaded (lockless) mode is active.
    bool is_single_threaded() const noexcept
    {
        return single_threaded_;
    }

    /** Enable or disable single-threaded (lockless) mode.

        When enabled, all scheduler mutex and condition variable
        operations become no-ops. Cross-thread post() is
        undefined behavior.
    */
    void configure_single_threaded(bool v) noexcept
    {
        single_threaded_ = v;
        mutex_.set_enabled(!v);
        cond_.set_enabled(!v);
    }

protected:
    timer_service* timer_svc_ = nullptr;
    bool single_threaded_ = false;

    reactor_scheduler() = default;

    /** Drain completed_ops during shutdown.

        Pops all operations from the global queue and destroys them,
        skipping the task sentinel. Signals all waiting threads.
        Derived classes call this from their shutdown() override
        before performing platform-specific cleanup.
    */
    void shutdown_drain();

    /// RAII guard that re-inserts the task sentinel after `run_task`.
    struct task_cleanup
    {
        reactor_scheduler const* sched;
        lock_type* lock;
        context_type* ctx;
        ~task_cleanup();
    };

    mutable mutex_type mutex_{true};
    mutable event_type cond_{true};
    mutable op_queue completed_ops_;
    mutable std::atomic<std::int64_t> outstanding_work_{0};
    std::atomic<bool> stopped_{false};
    mutable std::atomic<bool> task_running_{false};
    mutable bool task_interrupted_ = false;

    // Runtime-configurable reactor tuning parameters.
    // Defaults match the library's built-in values.
    unsigned max_events_per_poll_   = 128;
    unsigned inline_budget_initial_ = 2;
    unsigned inline_budget_max_     = 16;
    unsigned unassisted_budget_     = 4;

    /// Bit 0 of `state_`: set when the condvar should be signaled.
    static constexpr std::size_t signaled_bit = 1;

    /// Increment per waiting thread in `state_`.
    static constexpr std::size_t waiter_increment = 2;
    mutable std::size_t state_                    = 0;

    /// Sentinel op that triggers a reactor poll when dequeued.
    struct task_op final : scheduler_op
    {
        void operator()() override {}
        void destroy() override {}
    };
    task_op task_op_;

    /// Run the platform-specific reactor poll.
    virtual void
    run_task(lock_type& lock, context_type* ctx,
        long timeout_us) = 0;

    /// Wake a blocked reactor (e.g. write to eventfd or pipe).
    virtual void interrupt_reactor() const = 0;

private:
    struct work_cleanup
    {
        reactor_scheduler* sched;
        lock_type* lock;
        context_type* ctx;
        ~work_cleanup();
    };

    std::size_t do_one(
        lock_type& lock, long timeout_us, context_type* ctx);

    void signal_all(lock_type& lock) const;
    bool maybe_unlock_and_signal_one(lock_type& lock) const;
    bool unlock_and_signal_one(lock_type& lock) const;
    void clear_signal() const;
    void wait_for_signal(lock_type& lock) const;
    void wait_for_signal_for(
        lock_type& lock, long timeout_us) const;
    void wake_one_thread_and_unlock(lock_type& lock) const;
};

/** RAII guard that pushes/pops a scheduler context frame.

    On construction, pushes a new context frame onto the
    thread-local stack. On destruction, drains any remaining
    private queue items to the global queue and pops the frame.
*/
struct reactor_thread_context_guard
{
    /// The context frame managed by this guard.
    reactor_scheduler_context frame_;

    /// Construct the guard, pushing a frame for @a sched.
    explicit reactor_thread_context_guard(
        reactor_scheduler const* sched) noexcept
        : frame_(sched, reactor_context_stack.get())
    {
        reactor_context_stack.set(&frame_);
    }

    /// Destroy the guard, draining private work and popping the frame.
    ~reactor_thread_context_guard() noexcept
    {
        if (!frame_.private_queue.empty())
            frame_.key->drain_thread_queue(
                frame_.private_queue, frame_.private_outstanding_work);
        reactor_context_stack.set(frame_.next);
    }
};

// ---- Inline implementations ------------------------------------------------

inline
reactor_scheduler_context::reactor_scheduler_context(
    reactor_scheduler const* k,
    reactor_scheduler_context* n)
    : key(k)
    , next(n)
    , private_outstanding_work(0)
    , inline_budget(0)
    , inline_budget_max(
          static_cast<int>(k->inline_budget_initial()))
    , unassisted(false)
{
}

inline void
reactor_scheduler::configure_reactor(
    unsigned max_events,
    unsigned budget_init,
    unsigned budget_max,
    unsigned unassisted)
{
    if (max_events < 1 ||
        max_events > static_cast<unsigned>(std::numeric_limits<int>::max()))
        throw std::out_of_range(
            "max_events_per_poll must be in [1, INT_MAX]");
    if (budget_max > static_cast<unsigned>(std::numeric_limits<int>::max()))
        throw std::out_of_range(
            "inline_budget_max must be in [0, INT_MAX]");

    // Clamp initial and unassisted to budget_max.
    if (budget_init > budget_max)
        budget_init = budget_max;
    if (unassisted > budget_max)
        unassisted = budget_max;

    max_events_per_poll_   = max_events;
    inline_budget_initial_ = budget_init;
    inline_budget_max_     = budget_max;
    unassisted_budget_     = unassisted;
}

inline void
reactor_scheduler::reset_inline_budget() const noexcept
{
    // When budget is disabled (max==0), all paths below would no-op
    // (inline_budget stays 0). Skip the TLS lookup entirely.
    if (inline_budget_max_ == 0)
        return;
    if (auto* ctx = reactor_find_context(this))
    {
        // Cap when no other thread absorbed queued work
        if (ctx->unassisted)
        {
            ctx->inline_budget_max =
                static_cast<int>(unassisted_budget_);
            ctx->inline_budget =
                static_cast<int>(unassisted_budget_);
            return;
        }
        // Ramp up when previous cycle fully consumed budget.
        // max(1, ...) ensures the doubling escapes zero.
        if (ctx->inline_budget == 0)
            ctx->inline_budget_max = (std::min)(
                (std::max)(1, ctx->inline_budget_max) * 2,
                static_cast<int>(inline_budget_max_));
        else if (ctx->inline_budget < ctx->inline_budget_max)
            ctx->inline_budget_max =
                static_cast<int>(inline_budget_initial_);
        ctx->inline_budget = ctx->inline_budget_max;
    }
}

inline bool
reactor_scheduler::try_consume_inline_budget() const noexcept
{
    if (inline_budget_max_ == 0)
        return false;
    if (auto* ctx = reactor_find_context(this))
    {
        if (ctx->inline_budget > 0)
        {
            --ctx->inline_budget;
            return true;
        }
    }
    return false;
}

inline void
reactor_scheduler::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;

        explicit post_handler(std::coroutine_handle<> h) : h_(h) {}
        ~post_handler() override = default;

        void operator()() override
        {
            auto saved = h_;
            delete this;
            // Ensure stores from the posting thread are visible
            std::atomic_thread_fence(std::memory_order_acquire);
            saved.resume();
        }

        void destroy() override
        {
            auto saved = h_;
            delete this;
            saved.destroy();
        }
    };

    auto ph = std::make_unique<post_handler>(h);

    if (auto* ctx = reactor_find_context(this))
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(ph.release());
        return;
    }

    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    lock_type lock(mutex_);
    completed_ops_.push(ph.release());
    wake_one_thread_and_unlock(lock);
}

inline void
reactor_scheduler::post(scheduler_op* h) const
{
    if (auto* ctx = reactor_find_context(this))
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(h);
        return;
    }

    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    lock_type lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

inline bool
reactor_scheduler::running_in_this_thread() const noexcept
{
    return reactor_find_context(this) != nullptr;
}

inline void
reactor_scheduler::stop()
{
    lock_type lock(mutex_);
    if (!stopped_.load(std::memory_order_acquire))
    {
        stopped_.store(true, std::memory_order_release);
        signal_all(lock);
        interrupt_reactor();
    }
}

inline bool
reactor_scheduler::stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

inline void
reactor_scheduler::restart()
{
    stopped_.store(false, std::memory_order_release);
}

inline std::size_t
reactor_scheduler::run()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    reactor_thread_context_guard ctx(this);
    lock_type lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, -1, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

inline std::size_t
reactor_scheduler::run_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    reactor_thread_context_guard ctx(this);
    lock_type lock(mutex_);
    return do_one(lock, -1, &ctx.frame_);
}

inline std::size_t
reactor_scheduler::wait_one(long usec)
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    reactor_thread_context_guard ctx(this);
    lock_type lock(mutex_);
    return do_one(lock, usec, &ctx.frame_);
}

inline std::size_t
reactor_scheduler::poll()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    reactor_thread_context_guard ctx(this);
    lock_type lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, 0, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

inline std::size_t
reactor_scheduler::poll_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    reactor_thread_context_guard ctx(this);
    lock_type lock(mutex_);
    return do_one(lock, 0, &ctx.frame_);
}

inline void
reactor_scheduler::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
reactor_scheduler::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

inline void
reactor_scheduler::compensating_work_started() const noexcept
{
    auto* ctx = reactor_find_context(this);
    if (ctx)
        ++ctx->private_outstanding_work;
}

inline void
reactor_scheduler::drain_thread_queue(
    op_queue& queue, std::int64_t count) const
{
    if (count > 0)
        outstanding_work_.fetch_add(count, std::memory_order_relaxed);

    lock_type lock(mutex_);
    completed_ops_.splice(queue);
    if (count > 0)
        maybe_unlock_and_signal_one(lock);
}

inline void
reactor_scheduler::post_deferred_completions(op_queue& ops) const
{
    if (ops.empty())
        return;

    if (auto* ctx = reactor_find_context(this))
    {
        ctx->private_queue.splice(ops);
        return;
    }

    lock_type lock(mutex_);
    completed_ops_.splice(ops);
    wake_one_thread_and_unlock(lock);
}

inline void
reactor_scheduler::shutdown_drain()
{
    lock_type lock(mutex_);

    while (auto* h = completed_ops_.pop())
    {
        if (h == &task_op_)
            continue;
        lock.unlock();
        h->destroy();
        lock.lock();
    }

    signal_all(lock);
}

inline void
reactor_scheduler::signal_all(lock_type&) const
{
    state_ |= signaled_bit;
    cond_.notify_all();
}

inline bool
reactor_scheduler::maybe_unlock_and_signal_one(
    lock_type& lock) const
{
    state_ |= signaled_bit;
    if (state_ > signaled_bit)
    {
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    return false;
}

inline bool
reactor_scheduler::unlock_and_signal_one(
    lock_type& lock) const
{
    state_ |= signaled_bit;
    bool have_waiters = state_ > signaled_bit;
    lock.unlock();
    if (have_waiters)
        cond_.notify_one();
    return have_waiters;
}

inline void
reactor_scheduler::clear_signal() const
{
    state_ &= ~signaled_bit;
}

inline void
reactor_scheduler::wait_for_signal(
    lock_type& lock) const
{
    while ((state_ & signaled_bit) == 0)
    {
        state_ += waiter_increment;
        cond_.wait(lock);
        state_ -= waiter_increment;
    }
}

inline void
reactor_scheduler::wait_for_signal_for(
    lock_type& lock, long timeout_us) const
{
    if ((state_ & signaled_bit) == 0)
    {
        state_ += waiter_increment;
        cond_.wait_for(lock, std::chrono::microseconds(timeout_us));
        state_ -= waiter_increment;
    }
}

inline void
reactor_scheduler::wake_one_thread_and_unlock(
    lock_type& lock) const
{
    if (maybe_unlock_and_signal_one(lock))
        return;

    if (task_running_.load(std::memory_order_relaxed) && !task_interrupted_)
    {
        task_interrupted_ = true;
        lock.unlock();
        interrupt_reactor();
    }
    else
    {
        lock.unlock();
    }
}

inline reactor_scheduler::work_cleanup::~work_cleanup()
{
    if (ctx)
    {
        std::int64_t produced = ctx->private_outstanding_work;
        if (produced > 1)
            sched->outstanding_work_.fetch_add(
                produced - 1, std::memory_order_relaxed);
        else if (produced < 1)
            sched->work_finished();
        ctx->private_outstanding_work = 0;

        if (!ctx->private_queue.empty())
        {
            lock->lock();
            sched->completed_ops_.splice(ctx->private_queue);
        }
    }
    else
    {
        sched->work_finished();
    }
}

inline reactor_scheduler::task_cleanup::~task_cleanup()
{
    if (!ctx)
        return;

    if (ctx->private_outstanding_work > 0)
    {
        sched->outstanding_work_.fetch_add(
            ctx->private_outstanding_work, std::memory_order_relaxed);
        ctx->private_outstanding_work = 0;
    }

    if (!ctx->private_queue.empty())
    {
        if (!lock->owns_lock())
            lock->lock();
        sched->completed_ops_.splice(ctx->private_queue);
    }
}

inline std::size_t
reactor_scheduler::do_one(
    lock_type& lock, long timeout_us, context_type* ctx)
{
    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        scheduler_op* op = completed_ops_.pop();

        // Handle reactor sentinel — time to poll for I/O
        if (op == &task_op_)
        {
            bool more_handlers =
                !completed_ops_.empty() || (ctx && !ctx->private_queue.empty());

            if (!more_handlers &&
                (outstanding_work_.load(std::memory_order_acquire) == 0 ||
                 timeout_us == 0))
            {
                completed_ops_.push(&task_op_);
                return 0;
            }

            long task_timeout_us = more_handlers ? 0 : timeout_us;
            task_interrupted_ = task_timeout_us == 0;
            task_running_.store(true, std::memory_order_release);

            if (more_handlers)
                unlock_and_signal_one(lock);

            try
            {
                run_task(lock, ctx, task_timeout_us);
            }
            catch (...)
            {
                task_running_.store(false, std::memory_order_relaxed);
                throw;
            }

            task_running_.store(false, std::memory_order_relaxed);
            completed_ops_.push(&task_op_);
            if (timeout_us > 0)
                return 0;
            continue;
        }

        // Handle operation
        if (op != nullptr)
        {
            bool more = !completed_ops_.empty();

            if (more)
                ctx->unassisted = !unlock_and_signal_one(lock);
            else
            {
                ctx->unassisted = false;
                lock.unlock();
            }

            work_cleanup on_exit{this, &lock, ctx};
            (void)on_exit;

            (*op)();
            return 1;
        }

        // Try private queue before blocking
        if (reactor_drain_private_queue(ctx, outstanding_work_, completed_ops_))
            continue;

        if (outstanding_work_.load(std::memory_order_acquire) == 0 ||
            timeout_us == 0)
            return 0;

        clear_signal();
        if (timeout_us < 0)
            wait_for_signal(lock);
        else
            wait_for_signal_for(lock, timeout_us);
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SCHEDULER_HPP
