//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CAPY_EXECUTOR_WORK_HPP
#define CAPY_EXECUTOR_WORK_HPP

#include <capy/config.hpp>

namespace capy {

//------------------------------------------------

/** Abstract base class for executable work items.

    Work items are objects that can be queued for later execution.
    They form the foundation of the async operation model, allowing
    callbacks and coroutine resumptions to be posted to an executor
    for deferred invocation.

    Work items may be heap-allocated or may be data members of an
    enclosing object. The allocation strategy is determined by the
    creator of the work item.

    Derived classes must implement two member functions:

    @li `operator()` - Executes the work. For heap-allocated work
        items, `operator()` is responsible for deleting itself
        (typically via `delete this`) before returning.

    @li `destroy()` - Destroys an uninvoked work item. This is called
        when a queued work item must be discarded without execution,
        such as during shutdown or exception cleanup. For heap-allocated
        work items, this typically calls `delete this`.

    Work items must never be deleted directly with `delete`; use
    `operator()` for normal execution or `destroy()` for cleanup.

    @note Heap-allocated work items are typically allocated with
    custom allocators (such as op_cache or frame_pool) to minimize
    allocation overhead in high-frequency async operations.

    @note Some work items (such as those owned by containers like
    `std::unique_ptr` or embedded as data members) are not meant to
    be destroyed and should implement both functions as no-ops
    (for `operator()`, perform the work but don't delete).

    @see executor_work_queue
*/
class executor_work
{
public:
    virtual void operator()() = 0;
    virtual void destroy() = 0;

protected:
    ~executor_work() = default;

private:
    friend struct executor_work_queue;
    executor_work* next_ = nullptr;
};

/** An intrusive FIFO queue of work items.

    This queue manages work items using an intrusive singly-linked list,
    avoiding additional allocations for queue nodes. Work items are
    executed in the order they were pushed (first-in, first-out).

    The queue takes ownership of pushed work items and will delete
    any remaining items when destroyed.

    @note This is not thread-safe. External synchronization is required
    for concurrent access.

    @see executor_work
*/
struct executor_work_queue
{
    executor_work_queue() = default;

    executor_work_queue(executor_work_queue&& other) noexcept
        : head_(other.head_),
          tail_(other.tail_)
    {
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    executor_work_queue& operator=(executor_work_queue&&) = delete;

    ~executor_work_queue()
    {
        while(head_)
        {
            auto p = head_;
            head_ = head_->next_;
            p->destroy();
        }
    }

    bool empty() const noexcept { return head_ == nullptr; }

    void push(executor_work* p)
    {
        if(tail_)
        {
            tail_->next_ = p;
            tail_ = p;
            return;
        }
        head_ = p;
        tail_ = p;
    }

    void push(executor_work_queue& other)
    {
        if(other.empty())
            return;
        if(tail_)
        {
            tail_->next_ = other.head_;
            tail_ = other.tail_;
        }
        else
        {
            head_ = other.head_;
            tail_ = other.tail_;
        }
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    executor_work* pop()
    {
        if(head_)
        {
            auto p = head_;
            head_ = head_->next_;
            if(!head_)
                tail_ = nullptr;
            return p;
        }
        return nullptr;
    }

private:
    executor_work* head_ = nullptr;
    executor_work* tail_ = nullptr;
};

} // namespace capy

#endif
