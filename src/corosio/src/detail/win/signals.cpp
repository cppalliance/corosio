//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_SIGNAL_WIN)

#include "src/detail/win/signals.hpp"
#include "src/detail/iocp/scheduler.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/error.hpp>

#include <csignal>
#include <mutex>

/*
    Windows Signal Handling Implementation
    ======================================

    This file implements POSIX-style signal handling on Windows, integrated with
    the IOCP scheduler. Windows lacks native async signal support, so we use the
    C standard library's signal() function and manually bridge signals into the
    completion-based I/O model.

    Architecture Overview
    ---------------------

    Three layers manage signal registrations:

    1. signal_state (global singleton)
       - Tracks the global service list and per-signal registration counts
       - Owns the mutex that protects signal handler installation/removal
       - Multiple execution_contexts share this; each gets a win_signals entry

    2. win_signals (one per execution_context)
       - Maintains registrations_[] table indexed by signal number
       - Each slot is a doubly-linked list of all signal_registrations for that signal
       - Also maintains impl_list_ of all win_signal_impl objects it owns

    3. win_signal_impl (one per signal_set)
       - Owns a singly-linked list (sorted by signal number) of signal_registrations
       - Contains the pending_op_ used for async_wait operations

    The signal_registration struct links these together:
       - next_in_set / (implicit via sorted order): links registrations within one signal_set
       - prev_in_table / next_in_table: links registrations for the same signal across sets

    Signal Delivery Flow
    --------------------

    1. corosio_signal_handler() (C handler, must be async-signal-safe)
       - Called by the OS when a signal arrives
       - Delegates to deliver_signal() and re-registers itself (Windows resets to SIG_DFL)

    2. deliver_signal() broadcasts to all win_signals services:
       - If a signal_set is waiting (impl->waiting_ == true), complete it immediately
         by posting the signal_op to the scheduler
       - Otherwise, increment reg->undelivered to queue the signal for later

    3. start_wait() checks for queued signals first:
       - If undelivered > 0, consume one and post immediate completion
       - Otherwise, set waiting_ = true and call on_work_started() to keep context alive

    Locking Protocol
    ----------------

    Two mutex levels exist (must be acquired in this order to avoid deadlock):
       1. signal_state::mutex - protects handler registration and service list
       2. win_signals::mutex_ - protects per-service registration tables and wait state

    deliver_signal() acquires both locks because it iterates the global service list
    and modifies per-service state.

    Work Tracking
    -------------

    When waiting for a signal:
       - start_wait() calls sched_.on_work_started() to keep io_context::run() alive
       - signal_op::svc is set to point to the service
       - signal_op::operator()() calls work_finished() after resuming the coroutine

    If a signal was already queued (undelivered > 0), no work tracking is needed
    because completion is posted immediately.

    Signal Flags
    ------------

    Windows only supports `none` and `dont_care` flags. Any other flags
    (restart, no_child_stop, etc.) return `operation_not_supported`. The
    C runtime signal() function has no equivalent to sigaction() flags
    like SA_RESTART or SA_NOCLDSTOP.
*/

namespace boost::corosio {

namespace detail {

//------------------------------------------------------------------------------
//
// Global signal state
//
//------------------------------------------------------------------------------

namespace {

struct signal_state
{
    std::mutex mutex;
    win_signals* service_list = nullptr;
    std::size_t registration_count[max_signal_number] = {};
};

signal_state* get_signal_state()
{
    static signal_state state;
    return &state;
}

// C signal handler. Note: On POSIX this would need to be async-signal-safe,
// but Windows signal handling is synchronous (runs on the faulting thread)
// so we can safely acquire locks here.
extern "C" void corosio_signal_handler(int signal_number)
{
    win_signals::deliver_signal(signal_number);

    // Windows uses "one-shot" semantics: the handler reverts to SIG_DFL
    // after each delivery. Re-register to maintain our handler.
    ::signal(signal_number, corosio_signal_handler);
}

} // namespace

//------------------------------------------------------------------------------
//
// signal_op
//
//------------------------------------------------------------------------------

void
signal_op::
operator()()
{
    if (ec_out)
        *ec_out = {};
    if (signal_out)
        *signal_out = signal_number;

    // Capture svc before resuming: the coroutine may destroy this op,
    // so we cannot access any members after resume() returns
    auto* service = svc;
    svc = nullptr;

    d.dispatch(h).resume();

    // Balance the on_work_started() from start_wait. When svc is null
    // (immediate completion from queued signal), no work tracking occurred.
    if (service)
        service->work_finished();
}

void
signal_op::
destroy()
{
    // No-op: signal_op is embedded in win_signal_impl
}

//------------------------------------------------------------------------------
//
// win_signal_impl
//
//------------------------------------------------------------------------------

win_signal_impl::
win_signal_impl(win_signals& svc) noexcept
    : svc_(svc)
{
}

void
win_signal_impl::
release()
{
    // Clear all signals and cancel pending wait
    clear();
    cancel();
    svc_.destroy_impl(*this);
}

void
win_signal_impl::
wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    system::error_code* ec,
    int* signal_out)
{
    pending_op_.h = h;
    pending_op_.d = d;
    pending_op_.ec_out = ec;
    pending_op_.signal_out = signal_out;
    pending_op_.signal_number = 0;

    // Check for immediate cancellation
    if (token.stop_requested())
    {
        if (ec)
            *ec = make_error_code(capy::error::canceled);
        if (signal_out)
            *signal_out = 0;
        d.dispatch(h).resume();
        return;
    }

    svc_.start_wait(*this, &pending_op_);
}

system::result<void>
win_signal_impl::
add(int signal_number, signal_set::flags_t flags)
{
    return svc_.add_signal(*this, signal_number, flags);
}

system::result<void>
win_signal_impl::
remove(int signal_number)
{
    return svc_.remove_signal(*this, signal_number);
}

system::result<void>
win_signal_impl::
clear()
{
    return svc_.clear_signals(*this);
}

void
win_signal_impl::
cancel()
{
    svc_.cancel_wait(*this);
}

//------------------------------------------------------------------------------
//
// win_signals
//
//------------------------------------------------------------------------------

win_signals::
win_signals(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
{
    for (int i = 0; i < max_signal_number; ++i)
        registrations_[i] = nullptr;

    add_service(this);
}

win_signals::
~win_signals()
{
    remove_service(this);
}

void
win_signals::
shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = impl_list_.pop_front(); impl != nullptr;
         impl = impl_list_.pop_front())
    {
        // Clear registrations
        while (auto* reg = impl->signals_)
        {
            impl->signals_ = reg->next_in_set;
            delete reg;
        }
        delete impl;
    }
}

win_signal_impl&
win_signals::
create_impl()
{
    auto* impl = new win_signal_impl(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        impl_list_.push_back(impl);
    }

    return *impl;
}

void
win_signals::
destroy_impl(win_signal_impl& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        impl_list_.remove(&impl);
    }

    delete &impl;
}

system::result<void>
win_signals::
add_signal(
    win_signal_impl& impl,
    int signal_number,
    signal_set::flags_t flags)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

    // Windows only supports none and dont_care flags
    constexpr auto supported = signal_set::none | signal_set::dont_care;
    if ((flags & ~supported) != signal_set::none)
        return make_error_code(system::errc::operation_not_supported);

    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    // Check if already registered in this set
    signal_registration** insertion_point = &impl.signals_;
    signal_registration* reg = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        insertion_point = &reg->next_in_set;
        reg = reg->next_in_set;
    }

    if (reg && reg->signal_number == signal_number)
        return {}; // Already registered

    // Create new registration
    auto* new_reg = new signal_registration;
    new_reg->signal_number = signal_number;
    new_reg->owner = &impl;
    new_reg->undelivered = 0;

    // Register signal handler if first registration
    if (state->registration_count[signal_number] == 0)
    {
        if (::signal(signal_number, corosio_signal_handler) == SIG_ERR)
        {
            delete new_reg;
            return make_error_code(system::errc::invalid_argument);
        }
    }

    // Insert into set's registration list (sorted by signal number)
    new_reg->next_in_set = reg;
    *insertion_point = new_reg;

    // Insert into service's registration table
    new_reg->next_in_table = registrations_[signal_number];
    new_reg->prev_in_table = nullptr;
    if (registrations_[signal_number])
        registrations_[signal_number]->prev_in_table = new_reg;
    registrations_[signal_number] = new_reg;

    ++state->registration_count[signal_number];

    return {};
}

system::result<void>
win_signals::
remove_signal(
    win_signal_impl& impl,
    int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    // Find the registration in the set
    signal_registration** deletion_point = &impl.signals_;
    signal_registration* reg = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        deletion_point = &reg->next_in_set;
        reg = reg->next_in_set;
    }

    if (!reg || reg->signal_number != signal_number)
        return {}; // Not found, no-op

    // Restore default handler if last registration
    if (state->registration_count[signal_number] == 1)
    {
        if (::signal(signal_number, SIG_DFL) == SIG_ERR)
            return make_error_code(system::errc::invalid_argument);
    }

    // Remove from set's list
    *deletion_point = reg->next_in_set;

    // Remove from service's registration table
    if (registrations_[signal_number] == reg)
        registrations_[signal_number] = reg->next_in_table;
    if (reg->prev_in_table)
        reg->prev_in_table->next_in_table = reg->next_in_table;
    if (reg->next_in_table)
        reg->next_in_table->prev_in_table = reg->prev_in_table;

    --state->registration_count[signal_number];

    delete reg;
    return {};
}

system::result<void>
win_signals::
clear_signals(win_signal_impl& impl)
{
    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    system::error_code first_error;

    while (signal_registration* reg = impl.signals_)
    {
        int signal_number = reg->signal_number;

        // Restore default handler if last registration
        if (state->registration_count[signal_number] == 1)
        {
            if (::signal(signal_number, SIG_DFL) == SIG_ERR && !first_error)
                first_error = make_error_code(system::errc::invalid_argument);
        }

        // Remove from set's list
        impl.signals_ = reg->next_in_set;

        // Remove from service's registration table
        if (registrations_[signal_number] == reg)
            registrations_[signal_number] = reg->next_in_table;
        if (reg->prev_in_table)
            reg->prev_in_table->next_in_table = reg->next_in_table;
        if (reg->next_in_table)
            reg->next_in_table->prev_in_table = reg->prev_in_table;

        --state->registration_count[signal_number];

        delete reg;
    }

    if (first_error)
        return first_error;
    return {};
}

void
win_signals::
cancel_wait(win_signal_impl& impl)
{
    bool was_waiting = false;
    signal_op* op = nullptr;

    {
        std::lock_guard<win_mutex> lock(mutex_);
        if (impl.waiting_)
        {
            was_waiting = true;
            impl.waiting_ = false;
            op = &impl.pending_op_;
        }
    }

    if (was_waiting)
    {
        if (op->ec_out)
            *op->ec_out = make_error_code(capy::error::canceled);
        if (op->signal_out)
            *op->signal_out = 0;
        op->d.dispatch(op->h).resume();
        sched_.on_work_finished();
    }
}

void
win_signals::
start_wait(win_signal_impl& impl, signal_op* op)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);

        // Check for queued signals first
        signal_registration* reg = impl.signals_;
        while (reg)
        {
            if (reg->undelivered > 0)
            {
                --reg->undelivered;
                op->signal_number = reg->signal_number;
                op->svc = nullptr;  // No extra work_finished needed
                // Post for immediate completion - post() handles work tracking
                post(op);
                return;
            }
            reg = reg->next_in_set;
        }

        // No queued signals, wait for delivery
        // We call on_work_started() to keep io_context alive while waiting.
        // Set svc so signal_op::operator() will call work_finished().
        impl.waiting_ = true;
        op->svc = this;
        sched_.on_work_started();
    }
}

void
win_signals::
deliver_signal(int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return;

    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> lock(state->mutex);

    // Deliver to all services. We hold state->mutex while iterating, and
    // acquire each service's mutex_ inside (matching the lock order used by
    // add_signal/remove_signal) to safely read and modify registration state.
    win_signals* service = state->service_list;
    while (service)
    {
        std::lock_guard<win_mutex> svc_lock(service->mutex_);

        // Find registrations for this signal
        signal_registration* reg = service->registrations_[signal_number];
        while (reg)
        {
            win_signal_impl* impl = reg->owner;

            if (impl->waiting_)
            {
                // Complete the pending wait
                impl->waiting_ = false;
                impl->pending_op_.signal_number = signal_number;
                service->post(&impl->pending_op_);
            }
            else
            {
                // No waiter yet; increment undelivered so start_wait() will
                // find this signal immediately without blocking
                ++reg->undelivered;
            }

            reg = reg->next_in_table;
        }

        service = service->next_;
    }
}

void
win_signals::
work_started() noexcept
{
    sched_.work_started();
}

void
win_signals::
work_finished() noexcept
{
    sched_.work_finished();
}

void
win_signals::
post(signal_op* op)
{
    sched_.post(op);
}

void
win_signals::
add_service(win_signals* service)
{
    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> lock(state->mutex);

    service->next_ = state->service_list;
    service->prev_ = nullptr;
    if (state->service_list)
        state->service_list->prev_ = service;
    state->service_list = service;
}

void
win_signals::
remove_service(win_signals* service)
{
    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> lock(state->mutex);

    if (service->next_ || service->prev_ || state->service_list == service)
    {
        if (state->service_list == service)
            state->service_list = service->next_;
        if (service->prev_)
            service->prev_->next_ = service->next_;
        if (service->next_)
            service->next_->prev_ = service->prev_;
        service->next_ = nullptr;
        service->prev_ = nullptr;
    }
}

//------------------------------------------------------------------------------
//
// signal_set implementation (from signal_set.hpp)
//
//------------------------------------------------------------------------------

} // namespace detail

signal_set::
~signal_set()
{
    if (impl_)
        impl_->release();
}

signal_set::
signal_set(capy::execution_context& ctx)
    : io_object(ctx)
{
    impl_ = &ctx.use_service<detail::win_signals>().create_impl();
}

signal_set::
signal_set(signal_set&& other) noexcept
    : io_object(std::move(other))
{
    impl_ = other.impl_;
    other.impl_ = nullptr;
}

signal_set&
signal_set::
operator=(signal_set&& other)
{
    if (this != &other)
    {
        if (ctx_ != other.ctx_)
            detail::throw_logic_error("signal_set::operator=: context mismatch");

        if (impl_)
            impl_->release();

        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

system::result<void>
signal_set::
add(int signal_number, flags_t flags)
{
    return get().add(signal_number, flags);
}

system::result<void>
signal_set::
remove(int signal_number)
{
    return get().remove(signal_number);
}

system::result<void>
signal_set::
clear()
{
    return get().clear();
}

void
signal_set::
cancel()
{
    get().cancel();
}

} // namespace boost::corosio

#endif // _WIN32
