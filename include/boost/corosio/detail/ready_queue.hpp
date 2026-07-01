//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_READY_QUEUE_HPP
#define BOOST_COROSIO_DETAIL_READY_QUEUE_HPP

#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/continuation.hpp>

#include <bit>
#include <cstdint>

namespace boost::corosio::detail {

// A queue entry is a tagged pointer: low bit selects the node kind, the rest
// is the address. We steal a LOW bit (guaranteed zero by alignment), never a
// high bit (which would depend on a fragile platform canonical-address
// assumption). Both node types are >= 8-aligned, so the low 3 bits are free.
static_assert(alignof(scheduler_op) >= 2);
static_assert(alignof(capy::continuation) >= 2);
static_assert(sizeof(void*) == sizeof(std::uintptr_t));
static_assert(sizeof(capy::continuation::reserved) >= sizeof(void*));

inline constexpr std::uintptr_t ready_cont_bit = 1;

/// Return true if a queue entry refers to a continuation (vs a scheduler_op).
inline bool
ready_is_continuation(std::uintptr_t e) noexcept
{
    return (e & ready_cont_bit) != 0;
}

/// Recover the scheduler_op from an op-tagged entry.
inline scheduler_op*
ready_as_op(std::uintptr_t e) noexcept
{
    return std::bit_cast<scheduler_op*>(e & ~ready_cont_bit);
}

/// Recover the continuation from a continuation-tagged entry.
inline capy::continuation*
ready_as_cont(std::uintptr_t e) noexcept
{
    return std::bit_cast<capy::continuation*>(e & ~ready_cont_bit);
}

/** A unified intrusive FIFO of scheduler_ops and continuations.

    Carries both completion handlers (`scheduler_op`, dispatched via
    `(*op)()`) and posted coroutine resumptions (`capy::continuation`,
    dispatched via `h.resume()`) in one ordered queue, with no per-entry
    allocation. The next-link lives in the node: `scheduler_op::q_next_`
    for ops, `capy::continuation::reserved` for continuations.

    @par Thread Safety
    Not thread-safe; external synchronization required (the schedulers
    hold their dispatch mutex while touching it).
*/
class ready_queue
{
    std::uintptr_t head_ = 0;   // tagged first entry, 0 when empty
    std::uintptr_t tail_ = 0;   // tagged last entry, 0 when empty

    // Read a node's next-link by value. A continuation's link lives in its
    // void* `reserved` slot; bit_cast keeps us from forming a uintptr_t
    // lvalue over that void* object (which would violate strict aliasing).
    //
    // GCC 12/13 false-positive: when inlining proves an entry refers to a
    // continuation, -Warray-bounds still diagnoses the untaken scheduler_op
    // branch against the smaller object. Fixed in GCC 14.
    BOOST_COROSIO_GCC_WARNING_PUSH
    BOOST_COROSIO_GCC_WARNING_DISABLE("-Warray-bounds")
    static std::uintptr_t
    next_of(std::uintptr_t e) noexcept
    {
        if (ready_is_continuation(e))
            return std::bit_cast<std::uintptr_t>(ready_as_cont(e)->reserved);
        return ready_as_op(e)->q_next_;
    }

    static void
    set_next(std::uintptr_t e, std::uintptr_t nxt) noexcept
    {
        if (ready_is_continuation(e))
            ready_as_cont(e)->reserved = std::bit_cast<void*>(nxt);
        else
            ready_as_op(e)->q_next_ = nxt;
    }
    BOOST_COROSIO_GCC_WARNING_POP

    void
    push_entry(std::uintptr_t e) noexcept
    {
        set_next(e, 0);
        if (tail_)
            set_next(tail_, e);
        else
            head_ = e;
        tail_ = e;
    }

public:
    ready_queue() = default;

    ready_queue(ready_queue&& o) noexcept
        : head_(o.head_)
        , tail_(o.tail_)
    {
        o.head_ = 0;
        o.tail_ = 0;
    }

    ready_queue(ready_queue const&)            = delete;
    ready_queue& operator=(ready_queue const&) = delete;
    ready_queue& operator=(ready_queue&&)      = delete;

    /// Return true if the queue holds no entries.
    bool empty() const noexcept { return head_ == 0; }

    /// Append a scheduler_op to the back of the queue.
    void push(scheduler_op* op) noexcept
    {
        push_entry(std::bit_cast<std::uintptr_t>(op));
    }

    /// Append a continuation to the back of the queue.
    void push(capy::continuation& c) noexcept
    {
        push_entry(std::bit_cast<std::uintptr_t>(&c) | ready_cont_bit);
    }

    /// Move all entries of @p other to the back in O(1); @p other is emptied.
    void splice(ready_queue& other) noexcept
    {
        if (other.empty())
            return;
        if (tail_)
            set_next(tail_, other.head_);
        else
            head_ = other.head_;
        tail_ = other.tail_;
        other.head_ = 0;
        other.tail_ = 0;
    }

    /// Remove and return the front entry as a tagged value, or 0 when empty.
    std::uintptr_t pop() noexcept
    {
        auto e = head_;
        if (!e)
            return 0;
        head_ = next_of(e);
        if (!head_)
            tail_ = 0;
        return e;
    }
};

} // namespace boost::corosio::detail

#endif
