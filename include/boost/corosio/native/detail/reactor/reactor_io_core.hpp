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
#include <boost/corosio/native/detail/reactor/reactor_op_base.hpp>

#include <atomic>
#include <memory>
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
   for_each_desc_entry and op_to_desc_slot, and must derive from
   std::enable_shared_from_this<Derived>.
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
        auto self = self_ptr()->weak_from_this().lock();
        if (!self)
            return;

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
            op.impl_ptr = self;
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
        desc_state_.fd = fd;
        {
            std::lock_guard lock(desc_state_.mutex);
            self_ptr()->for_each_desc_entry(
                [](auto&, reactor_op_base*& slot) { slot = nullptr; });
        }
        if (auto ec = svc_.scheduler().register_descriptor(fd, &desc_state_))
        {
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

        std::lock_guard lock(desc_state_.mutex);
        bool io_done = false;
        if (ready_flag)
        {
            ready_flag = false;
            op.perform_io();
            io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
            if (!io_done)
                op.errn = 0;
        }

        if (io_done || op.cancelled.load(std::memory_order_acquire))
        {
            svc_.post(&op);
            svc_.work_finished();
            return;
        }

        if (desc_state_.unpollable)
        {
            // Nothing will ever report readiness for this fd.
            op.complete(EOPNOTSUPP, 0);
            svc_.post(&op);
            svc_.work_finished();
            return;
        }

        if (is_write_direction)
        {
            if (auto ec = svc_.scheduler().ensure_write_registered(
                    desc_state_.fd, &desc_state_))
            {
                op.complete(ec.value(), 0);
                svc_.post(&op);
                svc_.work_finished();
                return;
            }
        }

        desc_slot = &op;

        // Select rebuilds its fd_sets from parked ops only, so parking
        // must wake it. Compiled away for epoll and kqueue.
        if constexpr (Service::needs_park_notification)
            svc_.scheduler().notify_reactor();
    }

    /// Cancel every pending operation.
    void cancel_all() noexcept
    {
        auto self = self_ptr()->weak_from_this().lock();
        if (!self)
            return;

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
        post_claimed(claimed, count, self);
    }

    /** Cancel every operation and claim every parked one for teardown.

        Also clears the cached edge flags and, if the state is queued
        in the scheduler, pins the object alive until it is drained.
    */
    void abandon_all() noexcept
    {
        auto self = self_ptr()->weak_from_this().lock();
        if (!self)
            return;

        self_ptr()->for_each_op([](auto& op) { op.request_cancel(); });

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

            // Must be set under the same lock that invoke_deferred_io
            // clears is_enqueued_ under, or the object could be destroyed
            // while the scheduler still holds the queued descriptor_state.
            if (desc_state_.is_enqueued_.load(std::memory_order_acquire))
                desc_state_.impl_ref_ = self;
        }
        post_claimed(claimed, count, self);
    }

    /// Drop the reactor registration of @a fd and reset the state.
    void unregister_fd(int fd) noexcept
    {
        if (fd >= 0 && desc_state_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd);
        desc_state_.fd                = -1;
        desc_state_.registered_events = 0;
        desc_state_.unpollable        = false;
    }

private:
    // A claim empties its slot, so no more ops are claimed than
    // descriptor_state has slots (read, write, connect, wait_read,
    // wait_write, wait_error), however many ops share them.
    static constexpr int max_claimed = 6;

    void post_claimed(
        reactor_op_base** claimed,
        int count,
        std::shared_ptr<Derived> const& self) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            claimed[i]->impl_ptr = self;
            svc_.post(claimed[i]);
            svc_.work_finished();
        }
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_IO_CORE_HPP
