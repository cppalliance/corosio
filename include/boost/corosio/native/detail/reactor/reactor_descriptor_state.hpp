//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_STATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_STATE_HPP

#include <boost/corosio/native/detail/reactor/reactor_events.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>
#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/detail/ready_queue.hpp>

#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>

#include <atomic>
#include <cstdint>
#include <memory>

#include <errno.h>
#include <sys/socket.h>

namespace boost::corosio::detail {

/** Per-descriptor state shared across reactor backends.

    Tracks pending operations for a file descriptor. The fd is registered
    once with the reactor and stays registered until closed. Uses deferred
    I/O: the reactor sets ready_events atomically, then enqueues this state.
    When popped by the scheduler, invoke_deferred_io() performs I/O under
    the mutex and queues completed ops.

    Non-template: uses reactor_op_base pointers so the scheduler and
    descriptor_state code exist as a single copy in the binary regardless
    of how many backends are compiled in.

    @par Thread Safety
    The mutex protects operation pointers and ready flags. ready_events_
    and is_enqueued_ are atomic for lock-free reactor access.
*/
struct reactor_descriptor_state : scheduler_op
{
    /// Protects operation pointers and ready/cancel flags.
    /// Becomes a no-op in single-threaded mode.
    conditionally_enabled_mutex mutex{true};

    /// Pending read operation (guarded by `mutex`).
    reactor_op_base* read_op = nullptr;

    /// Pending write operation (guarded by `mutex`).
    reactor_op_base* write_op = nullptr;

    /// Pending connect operation (guarded by `mutex`).
    reactor_op_base* connect_op = nullptr;

    /// Pending wait-for-read operation (guarded by `mutex`).
    reactor_op_base* wait_read_op = nullptr;

    /// Pending wait-for-write operation (guarded by `mutex`).
    reactor_op_base* wait_write_op = nullptr;

    /// Pending wait-for-error operation (guarded by `mutex`).
    reactor_op_base* wait_error_op = nullptr;

    /// True if a read edge event arrived before an op was registered.
    bool read_ready = false;

    /// True if a write edge event arrived before an op was registered.
    bool write_ready = false;

    /// Event mask set during registration (no mutex needed).
    std::uint32_t registered_events = 0;

    /// File descriptor this state tracks.
    int fd = -1;

    /// Accumulated ready events (set by reactor, read by scheduler).
    std::atomic<std::uint32_t> ready_events_{0};

    /// True while this state is queued in the scheduler's completed_ops.
    std::atomic<bool> is_enqueued_{false};

    /// Owning scheduler for posting completions.
    reactor_scheduler const* scheduler_ = nullptr;

    /// Prevents impl destruction while queued in the scheduler.
    std::shared_ptr<void> impl_ref_;

    /// Add ready events atomically.
    /// Release pairs with the consumer's acquire exchange on
    /// ready_events_ so the consumer sees all flags. On x86 (TSO)
    /// this compiles to the same LOCK OR as relaxed.
    void add_ready_events(std::uint32_t ev) noexcept
    {
        ready_events_.fetch_or(ev, std::memory_order_release);
    }

    /// Invoke deferred I/O and dispatch completions.
    void operator()() override
    {
        invoke_deferred_io();
    }

    /// Destroy without invoking.
    /// Called during scheduler::shutdown() drain. Clear impl_ref_ to break
    /// the self-referential cycle set by close_socket().
    void destroy() override
    {
        impl_ref_.reset();
    }

    /** Perform deferred I/O and queue completions.

        Performs I/O under the mutex and queues completed ops. EAGAIN
        ops stay parked in their slot for re-delivery on the next
        edge event.
    */
    void invoke_deferred_io();
};

inline void
reactor_descriptor_state::invoke_deferred_io()
{
    std::shared_ptr<void> prevent_impl_destruction;
    ready_queue local_ops;

    {
        conditionally_enabled_mutex::scoped_lock lock(mutex);

        // Must clear is_enqueued_ and move impl_ref_ under the same
        // lock that processes I/O. close_socket() checks is_enqueued_
        // under this mutex — without atomicity between the flag store
        // and the ref move, close_socket() could see is_enqueued_==false,
        // skip setting impl_ref_, and destroy the impl under us.
        prevent_impl_destruction = std::move(impl_ref_);
        is_enqueued_.store(false, std::memory_order_release);

        std::uint32_t ev = ready_events_.exchange(0, std::memory_order_acquire);
        if (ev == 0)
        {
            // Mutex unlocks here; compensate for work_cleanup's decrement
            scheduler_->compensating_work_started();
            return;
        }

        int err = 0;
        if (ev & reactor_event_error)
        {
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                err = errno;
            // select raises its exceptional set for out-of-band/urgent
            // data as well as for genuine faults; on a healthy socket the
            // probe then reads SO_ERROR == 0. Faulting a pending read or
            // write on that is wrong, so an I/O operation completes only
            // on a real (non-zero) error. wait(error) still names a code
            // below.
        }

        if (ev & reactor_event_read)
        {
            if (read_op)
            {
                auto* rd = read_op;
                if (err)
                    rd->complete(err, 0);
                else
                    rd->perform_io();

                if (rd->errn == EAGAIN || rd->errn == EWOULDBLOCK)
                {
                    rd->errn = 0;
                }
                else
                {
                    read_op = nullptr;
                    local_ops.push(rd);
                }
            }
            else
            {
                read_ready = true;
            }

            // The event does not prove the socket is still readable: a
            // parked read op above may have drained it, or a speculative
            // read consumed the data before this dispatch ran. The wait
            // op's perform_io() re-probes and reports EAGAIN to stay
            // parked.
            if (wait_read_op)
            {
                auto* wo = wait_read_op;
                if (err)
                    wo->complete(err, 0);
                else
                    wo->perform_io();

                if (wo->errn == EAGAIN || wo->errn == EWOULDBLOCK)
                {
                    wo->errn = 0;
                }
                else
                {
                    wait_read_op = nullptr;
                    local_ops.push(wo);
                }
            }
        }
        if (ev & reactor_event_write)
        {
            bool had_write_op = (connect_op || write_op);
            // A writable event on a socket still in SYN_SENT (e.g. the
            // spurious pre-connect readiness of a fresh socket) must
            // not complete the connect; perform_io() reports EAGAIN
            // until a peer is actually established.
            if (connect_op)
            {
                auto* cn = connect_op;
                if (err)
                    cn->complete(err, 0);
                else
                    cn->perform_io();

                if (cn->errn == EAGAIN || cn->errn == EWOULDBLOCK)
                {
                    cn->errn = 0;
                }
                else
                {
                    connect_op = nullptr;
                    local_ops.push(cn);
                }
            }
            if (write_op)
            {
                auto* wr = write_op;
                if (err)
                    wr->complete(err, 0);
                else
                    wr->perform_io();

                if (wr->errn == EAGAIN || wr->errn == EWOULDBLOCK)
                {
                    wr->errn = 0;
                }
                else
                {
                    write_op = nullptr;
                    local_ops.push(wr);
                }
            }
            if (!had_write_op)
                write_ready = true;

            // Same re-probe discipline as the wait-for-read dispatch.
            if (wait_write_op)
            {
                auto* wo = wait_write_op;
                if (err)
                    wo->complete(err, 0);
                else
                    wo->perform_io();

                if (wo->errn == EAGAIN || wo->errn == EWOULDBLOCK)
                {
                    wo->errn = 0;
                }
                else
                {
                    wait_write_op = nullptr;
                    local_ops.push(wo);
                }
            }
        }
        // Complete a parked wait-for-error on any error condition.
        if ((ev & reactor_event_error) || err)
        {
            if (wait_error_op)
            {
                // wait(error) fired on the exceptional condition; name a
                // code even when the kernel exposed none (e.g. urgent
                // data leaves SO_ERROR == 0).
                int const werr = err ? err : EIO;
                wait_error_op->complete(werr, 0);
                local_ops.push(std::exchange(wait_error_op, nullptr));
            }
        }
        if (err)
        {
            if (read_op)
            {
                read_op->complete(err, 0);
                local_ops.push(std::exchange(read_op, nullptr));
            }
            if (write_op)
            {
                write_op->complete(err, 0);
                local_ops.push(std::exchange(write_op, nullptr));
            }
            if (connect_op)
            {
                connect_op->complete(err, 0);
                local_ops.push(std::exchange(connect_op, nullptr));
            }
            if (wait_read_op)
            {
                wait_read_op->complete(err, 0);
                local_ops.push(std::exchange(wait_read_op, nullptr));
            }
            if (wait_write_op)
            {
                wait_write_op->complete(err, 0);
                local_ops.push(std::exchange(wait_write_op, nullptr));
            }
        }
    }

    // Execute first handler inline — the scheduler's work_cleanup
    // accounts for this as the "consumed" work item. local_ops holds
    // only ops, so the popped entry decodes directly.
    scheduler_op* first = ready_as_op(local_ops.pop());
    if (first)
    {
        scheduler_->post_deferred_completions(local_ops);
        (*first)();
    }
    else
    {
        scheduler_->compensating_work_started();
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_STATE_HPP
