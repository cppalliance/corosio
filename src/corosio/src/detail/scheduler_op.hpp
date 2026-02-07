//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
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

#include <cstddef>
#include <cstdint>

namespace boost::corosio::detail {

/** Base class for completion handlers using function pointer dispatch.

    Handlers are continuations that execute after an asynchronous
    operation completes. They can be queued for deferred invocation,
    allowing callbacks and coroutine resumptions to be posted to an
    executor.

    This class uses a function pointer instead of virtual dispatch
    to minimize overhead in the completion path. Each derived class
    provides a static completion function that handles both normal
    invocation and destruction.

    @par Function Pointer Convention

    The func_type signature is:
    @code
    void(*)(void* owner, scheduler_op* op, std::uint32_t bytes, std::uint32_t error);
    @endcode

    - When owner != nullptr: Normal completion. Process the operation.
    - When owner == nullptr: Destroy mode. Clean up without invoking.

    @par Ownership Contract

    Callers must invoke exactly ONE of `complete()` or `destroy()`,
    never both.

    @see scheduler_op_queue
*/
class scheduler_op : public intrusive_queue<scheduler_op>::node
{
public:
    /** Function pointer type for completion handling.

        @param owner Pointer to the scheduler (nullptr for destroy).
        @param op The operation to complete or destroy.
        @param bytes Bytes transferred (for I/O operations).
        @param error Error code from the operation.
    */
    using func_type = void(*)(
        void* owner,
        scheduler_op* op,
        std::uint32_t bytes,
        std::uint32_t error);

    /** Complete the operation via function pointer (IOCP path).

        @param owner Pointer to the owning scheduler.
        @param bytes Bytes transferred.
        @param error Error code.
    */
    void complete(void* owner, std::uint32_t bytes, std::uint32_t error)
    {
        func_(owner, this, bytes, error);
    }

    /** Invoke the handler (epoll/select path).

        Override in derived classes to handle operation completion.
        Default implementation does nothing.
    */
    virtual void operator()() {}

    /** Destroy without invoking the handler.

        Called during shutdown or when discarding queued operations.
        Override in derived classes if cleanup is needed.
        Default implementation calls through func_ if set.
    */
    virtual void destroy()
    {
        if (func_)
            func_(nullptr, this, 0, 0);
    }

    virtual ~scheduler_op() = default;

protected:
    /** Default constructor for derived classes using virtual dispatch.

        Used by epoll/select backends that override operator() and destroy().
    */
    scheduler_op() noexcept
        : func_(nullptr)
    {
    }

    /** Construct with completion function for function pointer dispatch.

        Used by IOCP backend for non-virtual completion.

        @param func The static function to call for completion/destruction.
    */
    explicit scheduler_op(func_type func) noexcept
        : func_(func)
    {
    }

    func_type func_;

    // Pad to 32 bytes so derived structs (descriptor_state, epoll_op)
    // keep hot fields on optimal cache line boundaries
    std::byte reserved_[sizeof(void*)] = {};
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
    scheduler_op_queue() = default;

    scheduler_op_queue(scheduler_op_queue&& other) noexcept
        : q_(std::move(other.q_))
    {
    }

    scheduler_op_queue(scheduler_op_queue const&) = delete;
    scheduler_op_queue& operator=(scheduler_op_queue const&) = delete;
    scheduler_op_queue& operator=(scheduler_op_queue&&) = delete;

    ~scheduler_op_queue()
    {
        while(auto* h = q_.pop())
            h->destroy();
    }

    bool empty() const noexcept { return q_.empty(); }
    void push(scheduler_op* h) noexcept { q_.push(h); }
    void push(scheduler_op_queue& other) noexcept { q_.splice(other.q_); }
    scheduler_op* pop() noexcept { return q_.pop(); }
};

} // namespace boost::corosio::detail

#endif
