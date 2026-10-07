//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_IO_CORE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_IO_CORE_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>

#include <errno.h>

/* Shared reactor I/O protocol.

   One implementation of the register/park/cancel/teardown protocol
   for every reactor-backed object -- stream and datagram sockets,
   acceptors, and posix descriptors -- over a backend's
   descriptor_state. Asio keeps the same protocol in its reactor
   (start_op, cancel_ops, deregister_descriptor); services and the
   objects' own verbs stay per type.

   Derived supplies its op slots through for_each_op,
   for_each_desc_entry and op_to_desc_slot, and is an
   io_object::implementation whose callers hold a reference on it.
*/

namespace boost::corosio::detail {

/** CRTP base holding the reactor parking and cancel protocol.

    @tparam Derived   The concrete object type (CRTP).
    @tparam Service   The backend service that owns Derived.
    @tparam DescState The backend's descriptor_state type.
*/
template<class Derived, class Service, class DescState>
class reactor_io_core
{
    Derived* self_ptr() noexcept
    {
        return static_cast<Derived*>(this);
    }

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_io_core(Service& svc) noexcept : svc_(svc) {}

    Service& svc_;

public:
    /// Per-descriptor state for persistent reactor registration.
    DescState desc_state_;

    /** Cancel a single pending operation.

        Claims the operation from its descriptor_state slot under
        the mutex and posts it to the scheduler as cancelled.
    */
    template<class Op>
    void cancel_single_op(Op& op) noexcept
    {
        op.request_cancel();

        reactor_op_base** desc_op_ptr = self_ptr()->op_to_desc_slot(op);
        if (!desc_op_ptr)
            return;

        reactor_op_base* claimed = nullptr;
        {
            std::lock_guard lock(desc_state_.mutex);
            if (*desc_op_ptr == &op)
                claimed = std::exchange(*desc_op_ptr, nullptr);
            // Not in the slot: request_cancel() above already set
            // op.cancelled, which register_op consults before parking
            // and the completion decode consults on delivery. Latching
            // a descriptor flag here instead would outlive this op and
            // cancel the next wait in the same direction.
        }
        if (claimed)
        {
            op.object_ref_ = detail::object_ref(self_ptr());
            svc_.post(&op);
            svc_.work_finished();
        }
    }

protected:
    /** Clear every slot and register @a fd with the reactor.

        @return The reactor's refusal, in which case the state is left
            unregistered.
    */
    std::error_code register_fd(int fd) noexcept
    {
        {
            // A completion from an earlier registration may still read
            // these under the mutex.
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.fd = fd;
            self_ptr()->for_each_desc_entry(
                [](auto&, reactor_op_base*& slot) { slot = nullptr; });
        }
        if (auto ec = svc_.scheduler().register_descriptor(fd, &desc_state_))
        {
            std::lock_guard lock(desc_state_.mutex);
            desc_state_.fd                = -1;
            desc_state_.registered_events = 0;
            return ec;
        }
        return {};
    }

    /** Register an op with the reactor.

        Handles cached edge events. Called on the EAGAIN/EINPROGRESS
        path when speculative I/O failed.
    */
    template<class Op>
    void register_op(
        Op& op,
        reactor_op_base*& desc_slot,
        bool& ready_flag,
        bool is_write_direction = false) noexcept
    {
        svc_.work_started();
        if (park_op(op, desc_slot, ready_flag, is_write_direction))
        {
            // Select rebuilds its fd_sets from parked ops only, so
            // parking must wake it. Compiled away for epoll and kqueue.
            if constexpr (Service::needs_park_notification)
                svc_.scheduler().notify_reactor();
            return;
        }
        // Posted after the descriptor lock is released: the scheduler
        // takes its own lock before descriptor locks.
        svc_.post(&op);
        svc_.work_finished();
    }

    /// Cancel every pending operation.
    void cancel_all() noexcept
    {
        self_ptr()->for_each_op([](auto& op) { op.request_cancel(); });

        reactor_op_base* claimed[max_claimed];
        int count = 0;
        {
            std::lock_guard lock(desc_state_.mutex);
            self_ptr()->for_each_desc_entry(
                [&](auto& op, reactor_op_base*& desc_slot) {
                    if (desc_slot == &op)
                    {
                        BOOST_COROSIO_ASSERT(count < max_claimed);
                        claimed[count++] = std::exchange(desc_slot, nullptr);
                    }
                });
        }
        post_claimed(claimed, count);
    }

    /** Cancel every operation, drop the reactor registration, and
        claim every parked op for teardown.

        Deregistration hands a reference to the scheduler, which keeps
        the object alive while a reactor pass may still deliver an
        event for the descriptor. Afterwards the descriptor state reads
        as closed, so the caller may close or release the fd.
    */
    void abandon_all() noexcept
    {
        self_ptr()->for_each_op([](auto& op) { op.request_cancel(); });

        int fd          = -1;
        bool registered = false;
        {
            std::lock_guard lock(desc_state_.mutex);
            fd         = desc_state_.fd;
            registered = desc_state_.registered_events != 0;
        }
        if (fd >= 0 && registered)
        {
            svc_.scheduler().deregister_descriptor(fd);
            svc_.scheduler().retire_descriptor(
                desc_state_, detail::object_ref(self_ptr()));
        }

        reactor_op_base* claimed[max_claimed];
        int count = 0;
        {
            std::lock_guard lock(desc_state_.mutex);
            self_ptr()->for_each_desc_entry(
                [&](auto& /*op*/, reactor_op_base*& desc_slot) {
                    if (auto* c = std::exchange(desc_slot, nullptr))
                    {
                        BOOST_COROSIO_ASSERT(count < max_claimed);
                        claimed[count++] = c;
                    }
                });
            desc_state_.read_ready  = false;
            desc_state_.write_ready = false;
            // Cleared before the fd is closed: a queued completion reads
            // it under this mutex, and the number can be reused once
            // closed.
            desc_state_.fd                = -1;
            desc_state_.registered_events = 0;
            desc_state_.unpollable        = false;
        }
        post_claimed(claimed, count);
    }

    /** Reset the descriptor state a recycled object starts from.

        Every field is reset rather than asserted so a debug build may
        poison them on retirement. References and the queued flag are
        asserted: nothing may hold one once the count reached zero.
    */
    void reset_desc_state() noexcept
    {
        desc_state_.fd                = -1;
        desc_state_.registered_events = 0;
        desc_state_.unpollable        = false;
        desc_state_.read_op           = nullptr;
        desc_state_.write_op          = nullptr;
        desc_state_.connect_op        = nullptr;
        desc_state_.wait_read_op      = nullptr;
        desc_state_.wait_write_op     = nullptr;
        desc_state_.wait_error_op     = nullptr;
        desc_state_.read_ready        = false;
        desc_state_.write_ready       = false;
        BOOST_COROSIO_ASSERT(!desc_state_.object_ref_);
        BOOST_COROSIO_ASSERT(!desc_state_.retired_ref_);
        BOOST_COROSIO_ASSERT(!desc_state_.dropped_ref_);
        BOOST_COROSIO_ASSERT(
            !desc_state_.is_enqueued_.load(std::memory_order_relaxed));
    }

#if !defined(NDEBUG)
    /** Poison the descriptor fields `reset_desc_state()` re-initializes.

        Leaves the mutex, the atomics, `scheduler_`, the references and
        the retirement links alone: the scheduler and `recycle()` may
        still read them.
    */
    void poison_desc_state() noexcept
    {
        auto smash = [](auto& field) {
            std::memset(static_cast<void*>(&field), 0xDB, sizeof(field));
        };
        smash(desc_state_.fd);
        smash(desc_state_.registered_events);
        smash(desc_state_.unpollable);
        smash(desc_state_.read_op);
        smash(desc_state_.write_op);
        smash(desc_state_.connect_op);
        smash(desc_state_.wait_read_op);
        smash(desc_state_.wait_write_op);
        smash(desc_state_.wait_error_op);
        smash(desc_state_.read_ready);
        smash(desc_state_.write_ready);
    }
#endif

private:
    // A claim empties its slot, so no more ops are claimed than
    // descriptor_state has slots (read, write, connect, wait_read,
    // wait_write, wait_error), however many ops share them.
    static constexpr int max_claimed = 6;

    /** Park @a op unless it can complete now.

        @return True if parked; false if the caller must post it.
    */
    template<class Op>
    bool park_op(
        Op& op,
        reactor_op_base*& desc_slot,
        bool& ready_flag,
        bool is_write_direction) noexcept
    {
        std::lock_guard lock(desc_state_.mutex);
        if (ready_flag)
        {
            ready_flag = false;
            op.perform_io();
            if (op.errn != EAGAIN && op.errn != EWOULDBLOCK)
                return false;
            op.errn = 0;
        }

        if (op.cancelled.load(std::memory_order_acquire))
            return false;

        if (desc_state_.unpollable)
        {
            // Nothing will ever report readiness for this fd.
            op.complete(EOPNOTSUPP, 0);
            return false;
        }

        if (is_write_direction)
        {
            if (auto ec = svc_.scheduler().ensure_write_registered(
                    desc_state_.fd, &desc_state_))
            {
                op.complete(ec.value(), 0);
                return false;
            }
        }

        desc_slot = &op;
        return true;
    }

    void post_claimed(reactor_op_base** claimed, int count) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            claimed[i]->object_ref_ = detail::object_ref(self_ptr());
            svc_.post(claimed[i]);
            svc_.work_finished();
        }
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_IO_CORE_HPP
