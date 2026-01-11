//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/reactive_scheduler.hpp"
#include <boost/capy/thread_local_ptr.hpp>

namespace boost {
namespace corosio {
namespace detail {

namespace {

//------------------------------------------------------------------------------
// Thread-local call stack with private queue
//------------------------------------------------------------------------------

// Thread info contains the scheduler key and private queue
struct thread_info_impl
{
    scheduler const* key;
    capy::executor_work_queue private_queue;
    thread_info_impl* next;
};

// Thread-local head of the context stack
capy::thread_local_ptr<thread_info_impl> context_stack;

// Find thread_info for a specific scheduler (for post fast-path)
thread_info_impl*
find_thread_info(scheduler const* sched) noexcept
{
    for (auto* c = context_stack.get(); c; c = c->next)
        if (c->key == sched)
            return c;
    return nullptr;
}

// RAII guard that pushes/pops a frame onto the context stack
struct thread_context_guard
{
    thread_info_impl frame_;

    explicit thread_context_guard(scheduler const* sched) noexcept
        : frame_{sched, {}, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        // Note: private_queue should be empty here (flushed by work_cleanup)
        // But executor_work_queue destructor will destroy() any stragglers
        context_stack.set(frame_.next);
    }

    thread_context_guard(thread_context_guard const&) = delete;
    thread_context_guard& operator=(thread_context_guard const&) = delete;
};

} // namespace

//------------------------------------------------------------------------------
// thread_info definition (matches forward declaration in header)
//------------------------------------------------------------------------------

template<bool isUnsafe>
struct reactive_scheduler<isUnsafe>::thread_info
{
    thread_info_impl* impl;
};

//------------------------------------------------------------------------------
// work_cleanup - flushes private queue after each handler (matching Asio)
//------------------------------------------------------------------------------

template<bool isUnsafe>
struct work_cleanup
{
    reactive_scheduler<isUnsafe>* sched_;
    std::unique_lock<std::mutex>* lock_;
    thread_info_impl* this_thread_;

    ~work_cleanup()
    {
        // Flush private queue back to main queue
        if (!this_thread_->private_queue.empty())
        {
            if constexpr (!isUnsafe)
            {
                lock_->lock();
            }
            sched_->queue_.push(this_thread_->private_queue);
        }
    }
};

//------------------------------------------------------------------------------
// task_cleanup - re-inserts task sentinel after reactor runs
//------------------------------------------------------------------------------

template<bool isUnsafe>
struct task_cleanup
{
    reactive_scheduler<isUnsafe>* sched_;
    std::unique_lock<std::mutex>* lock_;
    thread_info_impl* this_thread_;

    ~task_cleanup()
    {
        if constexpr (!isUnsafe)
        {
            lock_->lock();
        }
        sched_->task_interrupted_ = true;
        sched_->queue_.push(this_thread_->private_queue);
        sched_->queue_.push(&sched_->task_operation_);
    }
};

//------------------------------------------------------------------------------
// reactive_scheduler implementation
//------------------------------------------------------------------------------

template<bool isUnsafe>
reactive_scheduler<isUnsafe>::
reactive_scheduler(
    capy::execution_context&,
    unsigned concurrency_hint)
    : one_thread_(concurrency_hint == 1)
{
}

template<bool isUnsafe>
reactive_scheduler<isUnsafe>::
~reactive_scheduler()
{
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
shutdown()
{
    std::unique_lock<std::mutex> lock(mutex_);
    shutdown_ = true;

    // Destroy all pending work items (except task_operation_ sentinel)
    while (auto* w = queue_.pop())
    {
        if (w != &task_operation_)
            w->destroy();
    }

    outstanding_work_ = 0;
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
init_task()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutdown_ && !task_)
    {
        // TODO: task_ = &use_service<reactor>(context());
        // For now, just insert the sentinel
        queue_.push(&task_operation_);
        cv_.notify_one();
    }
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
post(capy::coro h) const
{
    struct coro_work : capy::executor_work
    {
        capy::coro h_;

        explicit coro_work(capy::coro h)
            : h_(h)
        {
        }

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

    post(new coro_work(h));
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
post(capy::executor_work* w) const
{
    // Fast path: if one_thread_ and we're inside run(), use private queue
    if (one_thread_)
    {
        if (auto* info = find_thread_info(this))
        {
            // Push to thread-local private queue (no lock, no alloc!)
            info->private_queue.push(w);
            return;
        }
    }

    // Normal path: lock and push to main queue
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const_cast<capy::executor_work_queue&>(queue_).push(w);
        ++const_cast<std::size_t&>(
            const_cast<reactive_scheduler*>(this)->outstanding_work_);
    }
    else
    {
        const_cast<capy::executor_work_queue&>(queue_).push(w);
        ++const_cast<std::size_t&>(
            const_cast<reactive_scheduler*>(this)->outstanding_work_);
    }

    cv_.notify_one();
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
defer(capy::coro h) const
{
    post(h);
}

template<bool isUnsafe>
bool
reactive_scheduler<isUnsafe>::
running_in_this_thread() const noexcept
{
    return find_thread_info(this) != nullptr;
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
on_work_started() noexcept
{
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++outstanding_work_;
    }
    else
    {
        ++outstanding_work_;
    }
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
on_work_finished() noexcept
{
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --outstanding_work_;
    }
    else
    {
        --outstanding_work_;
    }
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
stop()
{
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    else
    {
        stopped_ = true;
    }

    cv_.notify_all();

    // TODO: Interrupt reactor when implemented
    // if (task_ && !task_interrupted_)
    // {
    //     task_interrupted_ = true;
    //     task_->interrupt();
    // }
}

template<bool isUnsafe>
bool
reactive_scheduler<isUnsafe>::
stopped() const noexcept
{
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }
    else
    {
        return stopped_;
    }
}

template<bool isUnsafe>
void
reactive_scheduler<isUnsafe>::
restart()
{
    if constexpr (!isUnsafe)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = false;
    }
    else
    {
        stopped_ = false;
    }
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
do_run(
    std::unique_lock<std::mutex>& lock,
    thread_info& this_thread,
    std::size_t max_handlers)
{
    std::size_t count = 0;

    while (count < max_handlers && !stopped_)
    {
        if (queue_.empty())
            break;

        auto* work = queue_.pop();

        // Check if this is the task (reactor) sentinel
        if (work == &task_operation_)
        {
            // TODO: When reactor is implemented, this will run the reactor
            // if (task_)
            // {
            //     bool more_handlers = !queue_.empty();
            //     task_interrupted_ = more_handlers;
            //     lock.unlock();
            //     {
            //         task_cleanup<isUnsafe> on_exit{...};
            //         task_->run(more_handlers ? 0 : -1,
            //             this_thread.impl->private_queue);
            //     }
            //     continue;
            // }

            // For now, just re-insert sentinel and continue
            queue_.push(&task_operation_);
            continue;
        }

        --outstanding_work_;

        lock.unlock();

        {
            work_cleanup<isUnsafe> on_exit{
                const_cast<reactive_scheduler*>(this),
                &lock,
                this_thread.impl};
            (void)on_exit;

            // Execute handler (may post to private_queue)
            (*work)();
        }
        // work_cleanup flushes private_queue → queue_

        ++count;

        if constexpr (!isUnsafe)
        {
            lock.lock();
        }
    }

    return count;
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
run()
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::size_t total = 0;
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stopped_)
    {
        // Wait for work or stop
        cv_.wait(lock, [this] {
            return stopped_ || !queue_.empty();
        });

        if (stopped_)
            break;

        std::size_t n = do_run(lock, this_thread, static_cast<std::size_t>(-1));
        if (n == 0)
            break;
        total += n;
    }

    return total;
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
run_one()
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for work or stop
    cv_.wait(lock, [this] {
        return stopped_ || !queue_.empty();
    });

    if (stopped_)
        return 0;

    return do_run(lock, this_thread, 1);
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
run_one(long usec)
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for work, stop, or timeout
    bool ready = cv_.wait_for(lock, std::chrono::microseconds(usec), [this] {
        return stopped_ || !queue_.empty();
    });

    if (!ready || stopped_)
        return 0;

    return do_run(lock, this_thread, 1);
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
wait_one(long usec)
{
    std::unique_lock<std::mutex> lock(mutex_);

    if (stopped_)
        return 0;

    // Wait for work, stop, or timeout
    bool ready = cv_.wait_for(lock, std::chrono::microseconds(usec), [this] {
        return stopped_ || !queue_.empty();
    });

    if (!ready || stopped_)
        return 0;

    // Don't execute, just report availability
    return queue_.empty() ? 0 : 1;
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
run_for(std::chrono::steady_clock::duration rel_time)
{
    auto end_time = std::chrono::steady_clock::now() + rel_time;
    return run_until(end_time);
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
run_until(std::chrono::steady_clock::time_point abs_time)
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::size_t total = 0;
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stopped_)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= abs_time)
            break;

        // Wait for work, stop, or timeout
        bool ready = cv_.wait_until(lock, abs_time, [this] {
            return stopped_ || !queue_.empty();
        });

        if (!ready || stopped_)
            break;

        std::size_t n = do_run(lock, this_thread, static_cast<std::size_t>(-1));
        total += n;

        if (n == 0)
            break;
    }

    return total;
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
poll()
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::unique_lock<std::mutex> lock(mutex_);
    return do_run(lock, this_thread, static_cast<std::size_t>(-1));
}

template<bool isUnsafe>
std::size_t
reactive_scheduler<isUnsafe>::
poll_one()
{
    thread_context_guard guard(this);
    thread_info this_thread{context_stack.get()};

    std::unique_lock<std::mutex> lock(mutex_);
    return do_run(lock, this_thread, 1);
}

//------------------------------------------------------------------------------
// Explicit template instantiations
//------------------------------------------------------------------------------

template class reactive_scheduler<false>;  // Thread-safe version
template class reactive_scheduler<true>;   // Unsafe (single-thread) version

} // namespace detail
} // namespace corosio
} // namespace boost
