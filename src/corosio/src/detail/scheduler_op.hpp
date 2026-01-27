//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_SCHEDULER_OP_HPP
#define BOOST_COROSIO_DETAIL_SCHEDULER_OP_HPP

#include <boost/corosio/detail/config.hpp>
#include "src/detail/intrusive.hpp"

namespace boost::corosio::detail {

/** Abstract base class for completion handlers.

    Handlers are continuations that execute after an asynchronous
    operation completes. They can be queued for deferred invocation,
    allowing callbacks and coroutine resumptions to be posted to an
    executor.

    Handlers should execute quickly - typically just initiating
    another I/O operation or suspending on a foreign task. Heavy
    computation should be avoided in handlers to prevent blocking
    the event loop.

    Handlers may be heap-allocated or may be data members of an
    enclosing object. The allocation strategy is determined by the
    creator of the handler.

    @par Ownership Contract

    Callers must invoke exactly ONE of `operator()` or `destroy()`,
    never both:

    @li `operator()` - Invokes the handler. The handler is
        responsible for its own cleanup (typically `delete this`
        for heap-allocated handlers). The caller must not call
        `destroy()` after invoking this.

    @li `destroy()` - Destroys an uninvoked handler. This is
        called when a queued handler must be discarded without
        execution, such as during shutdown or exception cleanup.
        For heap-allocated handlers, this typically calls
        `delete this`.

    @par Exception Safety

    Implementations of `operator()` must perform cleanup before
    any operation that might throw. This ensures that if the handler
    throws, the exception propagates cleanly to the caller of
    `run()` without leaking resources. Typical pattern:

    @code
    void operator()() override
    {
        auto h = h_;
        delete this;    // cleanup FIRST
        h.resume();     // then resume (may throw)
    }
    @endcode

    This "delete-before-invoke" pattern also enables memory
    recycling - the handler's memory can be reused immediately
    by subsequent allocations.

    @note Callers must never delete handlers directly with `delete`;
    use `operator()` for normal invocation or `destroy()` for cleanup.

    @note Heap-allocated handlers are typically allocated with
    custom allocators to minimize allocation overhead in
    high-frequency async operations.

    @note Some handlers (such as those owned by containers like
    `std::unique_ptr` or embedded as data members) are not meant to
    be destroyed and should implement both functions as no-ops
    (for `operator()`, invoke the continuation but don't delete).

    @see scheduler_op_queue
*/
class scheduler_op : public intrusive_queue<scheduler_op>::node
{
public:
    virtual void operator()() = 0;
    virtual void destroy() = 0;

    /** Returns the user-defined data pointer.

        Derived classes may set this to store auxiliary data
        such as a pointer to the most-derived object.

        @par Postconditions
        @li Initially returns `nullptr` for newly constructed handlers.
        @li Returns the current value of `data_` if modified by a derived class.

        @return The user-defined data pointer, or `nullptr` if not set.
    */
    void* data() const noexcept
    {
        return data_;
    }

protected:
    ~scheduler_op() = default;

    void* data_ = nullptr;
};

//------------------------------------------------------------------------------

using op_queue = intrusive_queue<scheduler_op>;

//------------------------------------------------------------------------------

/** An intrusive FIFO queue of scheduler_ops.

    This queue stores scheduler_ops using an intrusive linked list,
    avoiding additional allocations for queue nodes. Scheduler_ops
    are popped in the order they were pushed (first-in, first-out).

    The destructor calls `destroy()` on any remaining scheduler_ops.

    @note This is not thread-safe. External synchronization is
    required for concurrent access.

    @see scheduler_op
*/
class scheduler_op_queue
{
    op_queue q_;

public:
    /** Default constructor.

        Creates an empty queue.

        @post `empty() == true`
    */
    scheduler_op_queue() = default;

    /** Move constructor.

        Takes ownership of all scheduler_ops from `other`,
        leaving `other` empty.

        @param other The queue to move from.

        @post `other.empty() == true`
    */
    scheduler_op_queue(scheduler_op_queue&& other) noexcept
        : q_(std::move(other.q_))
    {
    }

    scheduler_op_queue(scheduler_op_queue const&) = delete;
    scheduler_op_queue& operator=(scheduler_op_queue const&) = delete;
    scheduler_op_queue& operator=(scheduler_op_queue&&) = delete;

    /** Destructor.

        Calls `destroy()` on any remaining scheduler_ops in the queue.
    */
    ~scheduler_op_queue()
    {
        while(auto* h = q_.pop())
            h->destroy();
    }

    /** Return true if the queue is empty.

        @return `true` if the queue contains no scheduler_ops.
    */
    bool
    empty() const noexcept
    {
        return q_.empty();
    }

    /** Add a scheduler_op to the back of the queue.

        @param h Pointer to the scheduler_op to add.

        @pre `h` is not null and not already in a queue.
    */
    void
    push(scheduler_op* h) noexcept
    {
        q_.push(h);
    }

    /** Splice all scheduler_ops from another queue to the back.

        All scheduler_ops from `other` are moved to the back of this
        queue. After this call, `other` is empty.

        @param other The queue to splice from.

        @post `other.empty() == true`
    */
    void
    push(scheduler_op_queue& other) noexcept
    {
        q_.splice(other.q_);
    }

    /** Remove and return the front scheduler_op.

        @return Pointer to the front scheduler_op, or `nullptr`
            if the queue is empty.
    */
    scheduler_op*
    pop() noexcept
    {
        return q_.pop();
    }
};

} // namespace boost::corosio::detail

#endif
