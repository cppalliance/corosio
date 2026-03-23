//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNALS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNALS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_signal.hpp>

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/native/detail/iocp/win_mutex.hpp>
#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/capy/error.hpp>

#include <csignal>
#include <mutex>

#include <signal.h>

/*
    Windows Signal Implementation - Header
    ======================================

    This header declares the internal types for Windows signal handling.

    Key Differences from POSIX:
      - Uses C runtime signal() instead of sigaction() (Windows has no sigaction)
      - Only `none` and `dont_care` flags are supported; other flags return
        `operation_not_supported` (Windows has no equivalent to SA_* flags)
      - Windows resets handler to SIG_DFL after each signal, so we must re-register
      - Only supports: SIGINT, SIGTERM, SIGABRT, SIGFPE, SIGILL, SIGSEGV
      - max_signal_number is 32 (vs 64 on Linux)

    The data structures mirror the POSIX implementation for consistency:
      - signal_op, signal_registration, win_signal, win_signals

    Threading note: Windows signal handling is synchronous (runs on faulting
    thread), so we can safely acquire locks in the signal handler. This differs
    from POSIX where the handler must be async-signal-safe.
*/

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
       - Also maintains impl_list_ of all win_signal objects it owns

    3. win_signal (one per signal_set)
       - Owns a singly-linked list (sorted by signal number) of signal_registrations
       - Contains the pending_op_ used for wait operations

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
       - Otherwise, set waiting_ = true and call work_started() to keep context alive

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
       - start_wait() calls sched_.work_started() to keep io_context::run() alive
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

namespace boost::corosio::detail {

class win_scheduler;

/** Windows signal management service.

    This service owns all signal set implementations and coordinates
    their lifecycle. It provides:

    - Signal implementation allocation and deallocation
    - Signal registration via the C runtime signal() function
    - Global signal state management
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class BOOST_COROSIO_DECL win_signals final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = win_signals;

    io_object::implementation* construct() override;
    void destroy(io_object::implementation*) override;

    /** Construct the signal service.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_signals(capy::execution_context& ctx);

    /** Destroy the signal service. */
    ~win_signals();

    win_signals(win_signals const&)            = delete;
    win_signals& operator=(win_signals const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Destroy a signal implementation. */
    void destroy_impl(win_signal& impl);

    /** Add a signal to a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to register.
        @param flags The flags to apply (ignored on Windows).
        @return Success, or an error.
    */
    std::error_code
    add_signal(win_signal& impl, int signal_number, signal_set::flags_t flags);

    /** Remove a signal from a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to unregister.
        @return Success, or an error.
    */
    std::error_code remove_signal(win_signal& impl, int signal_number);

    /** Remove all signals from a signal set.

        @param impl The signal implementation to clear.
        @return Success, or an error.
    */
    std::error_code clear_signals(win_signal& impl);

    /** Cancel pending wait operations.

        @param impl The signal implementation to cancel.
    */
    void cancel_wait(win_signal& impl);

    /** Start a wait operation.

        @param impl The signal implementation.
        @param op The operation to start.
    */
    void start_wait(win_signal& impl, signal_op* op);

    /** Deliver a signal to all registered handlers.

        Called from the signal handler.

        @param signal_number The signal that occurred.
    */
    static void deliver_signal(int signal_number);

    /** Notify scheduler of pending work. */
    void work_started() noexcept;

    /** Notify scheduler that work completed. */
    void work_finished() noexcept;

    /** Post an operation for completion. */
    void post(signal_op* op);

private:
    static void add_service(win_signals* service);
    static void remove_service(win_signals* service);

    win_scheduler& sched_;
    win_mutex mutex_;
    intrusive_list<win_signal> impl_list_;

    // Per-signal registration table for this service
    signal_registration* registrations_[max_signal_number];

    // Linked list of services for global signal delivery
    win_signals* next_ = nullptr;
    win_signals* prev_ = nullptr;
};

//
// Global signal state
//

namespace signal_detail {

struct signal_state
{
    std::mutex mutex;
    win_signals* service_list                         = nullptr;
    std::size_t registration_count[max_signal_number] = {};
};

BOOST_COROSIO_DECL signal_state* get_signal_state();

// C signal handler. Note: On POSIX this would need to be async-signal-safe,
// but Windows signal handling is synchronous (runs on the faulting thread)
// so we can safely acquire locks here.
extern "C" inline void
corosio_signal_handler(int signal_number)
{
    win_signals::deliver_signal(signal_number);

    // Windows uses "one-shot" semantics: the handler reverts to SIG_DFL
    // after each delivery. Re-register to maintain our handler.
    ::signal(signal_number, corosio_signal_handler);
}

} // namespace signal_detail

//
// signal_op
//

inline signal_op::signal_op() noexcept : scheduler_op(&do_complete) {}

inline void
signal_op::do_complete(
    void* owner,
    scheduler_op* base,
    std::uint32_t /*bytes*/,
    std::uint32_t /*error*/)
{
    auto* op = static_cast<signal_op*>(base);

    // Destroy path - no-op: signal_op is embedded in win_signal
    if (!owner)
        return;

    if (op->ec_out)
        *op->ec_out = {};
    if (op->signal_out)
        *op->signal_out = op->signal_number;

    auto* service = op->svc;
    op->svc       = nullptr;

    op->cont.h = op->h;
    dispatch_coro(op->d, op->cont).resume();

    if (service)
        service->work_finished();
}

//
// win_signal
//

inline win_signal::win_signal(win_signals& svc) noexcept : svc_(svc) {}

inline std::coroutine_handle<>
win_signal::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    int* signal_out)
{
    pending_op_.h             = h;
    pending_op_.d             = d;
    pending_op_.ec_out        = ec;
    pending_op_.signal_out    = signal_out;
    pending_op_.signal_number = 0;

    // Check for immediate cancellation
    if (token.stop_requested())
    {
        if (ec)
            *ec = make_error_code(capy::error::canceled);
        if (signal_out)
            *signal_out = 0;
        pending_op_.cont.h = h;
        dispatch_coro(d, pending_op_.cont).resume();
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    svc_.start_wait(*this, &pending_op_);
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline std::error_code
win_signal::add(int signal_number, signal_set::flags_t flags)
{
    return svc_.add_signal(*this, signal_number, flags);
}

inline std::error_code
win_signal::remove(int signal_number)
{
    return svc_.remove_signal(*this, signal_number);
}

inline std::error_code
win_signal::clear()
{
    return svc_.clear_signals(*this);
}

inline void
win_signal::cancel()
{
    svc_.cancel_wait(*this);
}

//
// win_signals
//

inline win_signals::win_signals(capy::execution_context& ctx)
    : sched_(ctx.use_service<win_scheduler>())
{
    for (int i = 0; i < max_signal_number; ++i)
        registrations_[i] = nullptr;

    add_service(this);
}

inline win_signals::~win_signals()
{
    remove_service(this);
}

inline void
win_signals::shutdown()
{
    std::lock_guard<win_mutex> lock(mutex_);

    for (auto* impl = impl_list_.pop_front(); impl != nullptr;
         impl       = impl_list_.pop_front())
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

inline io_object::implementation*
win_signals::construct()
{
    auto* impl = new win_signal(*this);

    {
        std::lock_guard<win_mutex> lock(mutex_);
        impl_list_.push_back(impl);
    }

    return impl;
}

inline void
win_signals::destroy(io_object::implementation* p)
{
    auto& impl = static_cast<win_signal&>(*p);
    impl.clear();
    impl.cancel();
    destroy_impl(impl);
}

inline void
win_signals::destroy_impl(win_signal& impl)
{
    {
        std::lock_guard<win_mutex> lock(mutex_);
        impl_list_.remove(&impl);
    }

    delete &impl;
}

inline std::error_code
win_signals::add_signal(
    win_signal& impl, int signal_number, signal_set::flags_t flags)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(std::errc::invalid_argument);

    // Windows only supports none and dont_care flags
    constexpr auto supported = signal_set::none | signal_set::dont_care;
    if ((flags & ~supported) != signal_set::none)
        return make_error_code(std::errc::operation_not_supported);

    signal_detail::signal_state* state = signal_detail::get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    // Check if already registered in this set
    signal_registration** insertion_point = &impl.signals_;
    signal_registration* reg              = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        insertion_point = &reg->next_in_set;
        reg             = reg->next_in_set;
    }

    if (reg && reg->signal_number == signal_number)
        return {}; // Already registered

    // Create new registration
    auto* new_reg          = new signal_registration;
    new_reg->signal_number = signal_number;
    new_reg->owner         = &impl;
    new_reg->undelivered   = 0;

    // Register signal handler if first registration
    if (state->registration_count[signal_number] == 0)
    {
        if (::signal(signal_number, signal_detail::corosio_signal_handler) ==
            SIG_ERR)
        {
            delete new_reg;
            return make_error_code(std::errc::invalid_argument);
        }
    }

    // Insert into set's registration list (sorted by signal number)
    new_reg->next_in_set = reg;
    *insertion_point     = new_reg;

    // Insert into service's registration table
    new_reg->next_in_table = registrations_[signal_number];
    new_reg->prev_in_table = nullptr;
    if (registrations_[signal_number])
        registrations_[signal_number]->prev_in_table = new_reg;
    registrations_[signal_number] = new_reg;

    ++state->registration_count[signal_number];

    return {};
}

inline std::error_code
win_signals::remove_signal(win_signal& impl, int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(std::errc::invalid_argument);

    signal_detail::signal_state* state = signal_detail::get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    // Find the registration in the set
    signal_registration** deletion_point = &impl.signals_;
    signal_registration* reg             = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        deletion_point = &reg->next_in_set;
        reg            = reg->next_in_set;
    }

    if (!reg || reg->signal_number != signal_number)
        return {}; // Not found, no-op

    // Restore default handler if last registration
    if (state->registration_count[signal_number] == 1)
    {
        if (::signal(signal_number, SIG_DFL) == SIG_ERR)
            return make_error_code(std::errc::invalid_argument);
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

inline std::error_code
win_signals::clear_signals(win_signal& impl)
{
    signal_detail::signal_state* state = signal_detail::get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    std::error_code first_error;

    while (signal_registration* reg = impl.signals_)
    {
        int signal_number = reg->signal_number;

        // Restore default handler if last registration
        if (state->registration_count[signal_number] == 1)
        {
            if (::signal(signal_number, SIG_DFL) == SIG_ERR && !first_error)
                first_error = make_error_code(std::errc::invalid_argument);
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

inline void
win_signals::cancel_wait(win_signal& impl)
{
    bool was_waiting = false;
    signal_op* op    = nullptr;

    {
        std::lock_guard<win_mutex> lock(mutex_);
        if (impl.waiting_)
        {
            was_waiting   = true;
            impl.waiting_ = false;
            op            = &impl.pending_op_;
        }
    }

    if (was_waiting)
    {
        if (op->ec_out)
            *op->ec_out = make_error_code(capy::error::canceled);
        if (op->signal_out)
            *op->signal_out = 0;
        op->cont.h = op->h;
        dispatch_coro(op->d, op->cont).resume();
        sched_.work_finished();
    }
}

inline void
win_signals::start_wait(win_signal& impl, signal_op* op)
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
                op->svc           = nullptr; // No extra work_finished needed
                // Post for immediate completion - post() handles work tracking
                post(op);
                return;
            }
            reg = reg->next_in_set;
        }

        // No queued signals, wait for delivery
        // We call work_started() to keep io_context alive while waiting.
        // Set svc so signal_op::operator() will call work_finished().
        impl.waiting_ = true;
        op->svc       = this;
        sched_.work_started();
    }
}

inline void
win_signals::deliver_signal(int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return;

    signal_detail::signal_state* state = signal_detail::get_signal_state();
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
            win_signal* impl = reg->owner;

            if (impl->waiting_)
            {
                // Complete the pending wait
                impl->waiting_                  = false;
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

inline void
win_signals::work_started() noexcept
{
    sched_.work_started();
}

inline void
win_signals::work_finished() noexcept
{
    sched_.work_finished();
}

inline void
win_signals::post(signal_op* op)
{
    sched_.post(op);
}

inline void
win_signals::add_service(win_signals* service)
{
    signal_detail::signal_state* state = signal_detail::get_signal_state();
    std::lock_guard<std::mutex> lock(state->mutex);

    service->next_ = state->service_list;
    service->prev_ = nullptr;
    if (state->service_list)
        state->service_list->prev_ = service;
    state->service_list = service;
}

inline void
win_signals::remove_service(win_signals* service)
{
    signal_detail::signal_state* state = signal_detail::get_signal_state();
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

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNALS_HPP
