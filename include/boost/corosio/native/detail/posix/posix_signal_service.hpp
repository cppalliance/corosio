//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/posix/posix_signal.hpp>

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/capy/error.hpp>

#include <mutex>

#include <signal.h>

/*
    POSIX Signal Service
    ====================

    Concrete signal service implementation for POSIX backends. Manages signal
    registrations via sigaction() and dispatches completions through the
    scheduler. One instance per execution_context, created by
    get_signal_service().

    See the block comment further down for the full architecture overview.
*/

/*
    POSIX Signal Implementation
    ===========================

    This file implements signal handling for POSIX systems using sigaction().
    The implementation supports signal flags (SA_RESTART, etc.) and integrates
    with any POSIX-compatible scheduler via the abstract scheduler interface.

    Architecture Overview
    ---------------------

    Three layers manage signal registrations:

    1. signal_state (global singleton)
       - Tracks the global service list and per-signal registration counts
       - Stores the flags used for first registration of each signal (for
         conflict detection when multiple signal_sets register same signal)
       - Owns the mutex that protects signal handler installation/removal

    2. posix_signal_service (one per execution_context)
       - Maintains registrations_[] table indexed by signal number
       - Each slot is a doubly-linked list of signal_registrations for that signal
       - Also maintains impl_list_ of all posix_signal objects it owns

    3. posix_signal (one per signal_set)
       - Owns a singly-linked list (sorted by signal number) of signal_registrations
       - Contains the pending_op_ used for wait operations

    Signal Delivery Flow
    --------------------

    1. Signal arrives -> corosio_posix_signal_handler() (must be async-signal-safe)
       -> deliver_signal()

    2. deliver_signal() iterates all posix_signal_service services:
       - If a signal_set is waiting (impl->waiting_ == true), post the signal_op
         to the scheduler for immediate completion
       - Otherwise, increment reg->undelivered to queue the signal

    3. When wait() is called via start_wait():
       - First check for queued signals (undelivered > 0); if found, post
         immediate completion without blocking
       - Otherwise, set waiting_ = true and call work_started() to keep
         the io_context alive

    Locking Protocol
    ----------------

    Two mutex levels exist (MUST acquire in this order to avoid deadlock):
      1. signal_state::mutex - protects handler registration and service list
      2. posix_signal_service::mutex_ - protects per-service registration tables

    Async-Signal-Safety Limitation
    ------------------------------

    IMPORTANT: deliver_signal() is called from signal handler context and
    acquires mutexes. This is NOT strictly async-signal-safe per POSIX.
    The limitation:
      - If a signal arrives while another thread holds state->mutex or
        service->mutex_, and that same thread receives the signal, a
        deadlock can occur (self-deadlock on non-recursive mutex).

    This design trades strict async-signal-safety for implementation simplicity.
    In practice, deadlocks are rare because:
      - Mutexes are held only briefly during registration changes
      - Most programs don't modify signal sets while signals are expected
      - The window for signal arrival during mutex hold is small

    A fully async-signal-safe implementation would require lock-free data
    structures and atomic operations throughout, significantly increasing
    complexity.

    Flag Handling
    -------------

    - Flags are abstract values in the public API (signal_set::flags_t)
    - flags_supported() validates that requested flags are available on
      this platform; returns false if SA_NOCLDWAIT is unavailable and
      no_child_wait is requested
    - to_sigaction_flags() maps validated flags to actual SA_* constants
    - First registration of a signal establishes the flags; subsequent
      registrations must be compatible (same flags or dont_care)
    - Requesting unavailable flags returns operation_not_supported

    Work Tracking
    -------------

    When waiting for a signal:
      - start_wait() calls sched_->work_started() to prevent io_context::run()
        from returning while we wait
      - signal_op::svc is set to point to the service
      - signal_op::operator()() calls work_finished() after resuming the coroutine

    If a signal was already queued (undelivered > 0), no work tracking is needed
    because completion is posted immediately.
*/

namespace boost::corosio {

namespace detail {

/** Signal service for POSIX backends.

    Manages signal registrations via sigaction() and dispatches signal
    completions through the scheduler. One instance per execution_context.
*/
class BOOST_COROSIO_DECL posix_signal_service final
    : public capy::execution_context::service
    , public io_object::io_service
{
public:
    using key_type = posix_signal_service;

    posix_signal_service(capy::execution_context& ctx, scheduler& sched);
    ~posix_signal_service() override;

    posix_signal_service(posix_signal_service const&)            = delete;
    posix_signal_service& operator=(posix_signal_service const&) = delete;

    io_object::implementation* construct() override;

    void destroy(io_object::implementation* p) override
    {
        auto& impl              = static_cast<posix_signal&>(*p);
        [[maybe_unused]] auto n = impl.clear();
        impl.cancel();
        destroy_impl(impl);
    }

    void shutdown() override;

    void destroy_impl(posix_signal& impl);

    std::error_code add_signal(
        posix_signal& impl, int signal_number, signal_set::flags_t flags);

    std::error_code remove_signal(posix_signal& impl, int signal_number);

    std::error_code clear_signals(posix_signal& impl);

    void cancel_wait(posix_signal& impl);
    void start_wait(posix_signal& impl, signal_op* op);

    static void deliver_signal(int signal_number);

    void work_started() noexcept;
    void work_finished() noexcept;
    void post(signal_op* op);

private:
    static void add_service(posix_signal_service* service);
    static void remove_service(posix_signal_service* service);

    scheduler* sched_;
    std::mutex mutex_;
    intrusive_list<posix_signal> impl_list_;

    // Per-signal registration table
    signal_registration* registrations_[max_signal_number];

    // Registration counts for each signal
    std::size_t registration_count_[max_signal_number];

    // Linked list of all posix_signal_service services for signal delivery
    posix_signal_service* next_ = nullptr;
    posix_signal_service* prev_ = nullptr;
};

/** Get or create the signal service for the given context.

    This function is called by the concrete scheduler during initialization
    to create the signal service with a reference to itself.

    @param ctx Reference to the owning execution_context.
    @param sched Reference to the scheduler for posting completions.
    @return Reference to the signal service.
*/
posix_signal_service&
get_signal_service(capy::execution_context& ctx, scheduler& sched);

} // namespace detail

} // namespace boost::corosio

// ---------------------------------------------------------------------------
// Inline implementation
// ---------------------------------------------------------------------------

namespace boost::corosio {

namespace detail {

namespace posix_signal_detail {

struct signal_state
{
    std::mutex mutex;
    posix_signal_service* service_list                      = nullptr;
    std::size_t registration_count[max_signal_number]       = {};
    signal_set::flags_t registered_flags[max_signal_number] = {};
};

BOOST_COROSIO_DECL signal_state* get_signal_state();

// Check if requested flags are supported on this platform.
// Returns true if all flags are supported, false otherwise.
inline bool
flags_supported([[maybe_unused]] signal_set::flags_t flags)
{
#ifndef SA_NOCLDWAIT
    if (flags & signal_set::no_child_wait)
        return false;
#endif
    return true;
}

// Map abstract flags to sigaction() flags.
// Caller must ensure flags_supported() returns true first.
inline int
to_sigaction_flags(signal_set::flags_t flags)
{
    int sa_flags = 0;
    if (flags & signal_set::restart)
        sa_flags |= SA_RESTART;
    if (flags & signal_set::no_child_stop)
        sa_flags |= SA_NOCLDSTOP;
#ifdef SA_NOCLDWAIT
    if (flags & signal_set::no_child_wait)
        sa_flags |= SA_NOCLDWAIT;
#endif
    if (flags & signal_set::no_defer)
        sa_flags |= SA_NODEFER;
    if (flags & signal_set::reset_handler)
        sa_flags |= SA_RESETHAND;
    return sa_flags;
}

// Check if two flag values are compatible
inline bool
flags_compatible(signal_set::flags_t existing, signal_set::flags_t requested)
{
    // dont_care is always compatible
    if ((existing & signal_set::dont_care) ||
        (requested & signal_set::dont_care))
        return true;

    // Mask out dont_care bit for comparison
    constexpr auto mask = ~signal_set::dont_care;
    return (existing & mask) == (requested & mask);
}

// C signal handler - must be async-signal-safe
inline void
corosio_posix_signal_handler(int signal_number)
{
    posix_signal_service::deliver_signal(signal_number);
    // Note: With sigaction(), the handler persists automatically
    // (unlike some signal() implementations that reset to SIG_DFL)
}

} // namespace posix_signal_detail

// signal_op implementation

inline void
signal_op::operator()()
{
    if (ec_out)
        *ec_out = {};
    if (signal_out)
        *signal_out = signal_number;

    // Capture svc before resuming (coro may destroy us)
    auto* service = svc;
    svc           = nullptr;

    cont_op.cont.h = h;
    d.post(cont_op.cont);

    // Balance the work_started() from start_wait
    if (service)
        service->work_finished();
}

inline void
signal_op::destroy()
{
    // No-op: signal_op is embedded in posix_signal
}

// posix_signal implementation

inline posix_signal::posix_signal(posix_signal_service& svc) noexcept
    : svc_(svc)
{
}

inline std::coroutine_handle<>
posix_signal::wait(
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

    if (token.stop_requested())
    {
        if (ec)
            *ec = make_error_code(capy::error::canceled);
        if (signal_out)
            *signal_out = 0;
        pending_op_.cont_op.cont.h = h;
        d.post(pending_op_.cont_op.cont);
        // completion is always posted to scheduler queue, never inline.
        return std::noop_coroutine();
    }

    svc_.start_wait(*this, &pending_op_);
    // completion is always posted to scheduler queue, never inline.
    return std::noop_coroutine();
}

inline std::error_code
posix_signal::add(int signal_number, signal_set::flags_t flags)
{
    return svc_.add_signal(*this, signal_number, flags);
}

inline std::error_code
posix_signal::remove(int signal_number)
{
    return svc_.remove_signal(*this, signal_number);
}

inline std::error_code
posix_signal::clear()
{
    return svc_.clear_signals(*this);
}

inline void
posix_signal::cancel()
{
    svc_.cancel_wait(*this);
}

// posix_signal_service implementation

inline posix_signal_service::posix_signal_service(
    capy::execution_context&, scheduler& sched)
    : sched_(&sched)
{
    for (int i = 0; i < max_signal_number; ++i)
    {
        registrations_[i]      = nullptr;
        registration_count_[i] = 0;
    }
    add_service(this);
}

inline posix_signal_service::~posix_signal_service()
{
    remove_service(this);
}

inline void
posix_signal_service::shutdown()
{
    std::lock_guard lock(mutex_);

    for (auto* impl = impl_list_.pop_front(); impl != nullptr;
         impl       = impl_list_.pop_front())
    {
        while (auto* reg = impl->signals_)
        {
            impl->signals_ = reg->next_in_set;
            delete reg;
        }
        delete impl;
    }
}

inline io_object::implementation*
posix_signal_service::construct()
{
    auto* impl = new posix_signal(*this);

    {
        std::lock_guard lock(mutex_);
        impl_list_.push_back(impl);
    }

    return impl;
}

inline void
posix_signal_service::destroy_impl(posix_signal& impl)
{
    {
        std::lock_guard lock(mutex_);
        impl_list_.remove(&impl);
    }

    delete &impl;
}

inline std::error_code
posix_signal_service::add_signal(
    posix_signal& impl, int signal_number, signal_set::flags_t flags)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(std::errc::invalid_argument);

    // Validate that requested flags are supported on this platform
    // (e.g., SA_NOCLDWAIT may not be available on all POSIX systems)
    if (!posix_signal_detail::flags_supported(flags))
        return make_error_code(std::errc::operation_not_supported);

    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    // Find insertion point (list is sorted by signal number)
    signal_registration** insertion_point = &impl.signals_;
    signal_registration* reg              = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        insertion_point = &reg->next_in_set;
        reg             = reg->next_in_set;
    }

    // Already registered in this set - check flag compatibility
    // (same signal_set adding same signal twice with different flags)
    if (reg && reg->signal_number == signal_number)
    {
        if (!posix_signal_detail::flags_compatible(reg->flags, flags))
            return make_error_code(std::errc::invalid_argument);
        return {};
    }

    // Check flag compatibility with global registration
    // (different signal_set already registered this signal with different flags)
    if (state->registration_count[signal_number] > 0)
    {
        if (!posix_signal_detail::flags_compatible(
                state->registered_flags[signal_number], flags))
            return make_error_code(std::errc::invalid_argument);
    }

    auto* new_reg          = new signal_registration;
    new_reg->signal_number = signal_number;
    new_reg->flags         = flags;
    new_reg->owner         = &impl;
    new_reg->undelivered   = 0;

    // Install signal handler on first global registration
    if (state->registration_count[signal_number] == 0)
    {
        struct sigaction sa = {};
        sa.sa_handler       = posix_signal_detail::corosio_posix_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = posix_signal_detail::to_sigaction_flags(flags);

        if (::sigaction(signal_number, &sa, nullptr) < 0)
        {
            delete new_reg;
            return make_error_code(std::errc::invalid_argument);
        }

        // Store the flags used for first registration
        state->registered_flags[signal_number] = flags;
    }

    new_reg->next_in_set = reg;
    *insertion_point     = new_reg;

    new_reg->next_in_table = registrations_[signal_number];
    new_reg->prev_in_table = nullptr;
    if (registrations_[signal_number])
        registrations_[signal_number]->prev_in_table = new_reg;
    registrations_[signal_number] = new_reg;

    ++state->registration_count[signal_number];
    ++registration_count_[signal_number];

    return {};
}

inline std::error_code
posix_signal_service::remove_signal(posix_signal& impl, int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(std::errc::invalid_argument);

    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    signal_registration** deletion_point = &impl.signals_;
    signal_registration* reg             = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        deletion_point = &reg->next_in_set;
        reg            = reg->next_in_set;
    }

    if (!reg || reg->signal_number != signal_number)
        return {};

    // Restore default handler on last global unregistration
    if (state->registration_count[signal_number] == 1)
    {
        struct sigaction sa = {};
        sa.sa_handler       = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        if (::sigaction(signal_number, &sa, nullptr) < 0)
            return make_error_code(std::errc::invalid_argument);

        // Clear stored flags
        state->registered_flags[signal_number] = signal_set::none;
    }

    *deletion_point = reg->next_in_set;

    if (registrations_[signal_number] == reg)
        registrations_[signal_number] = reg->next_in_table;
    if (reg->prev_in_table)
        reg->prev_in_table->next_in_table = reg->next_in_table;
    if (reg->next_in_table)
        reg->next_in_table->prev_in_table = reg->prev_in_table;

    --state->registration_count[signal_number];
    --registration_count_[signal_number];

    delete reg;
    return {};
}

inline std::error_code
posix_signal_service::clear_signals(posix_signal& impl)
{
    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    std::error_code first_error;

    while (signal_registration* reg = impl.signals_)
    {
        int signal_number = reg->signal_number;

        if (state->registration_count[signal_number] == 1)
        {
            struct sigaction sa = {};
            sa.sa_handler       = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;

            if (::sigaction(signal_number, &sa, nullptr) < 0 && !first_error)
                first_error = make_error_code(std::errc::invalid_argument);

            // Clear stored flags
            state->registered_flags[signal_number] = signal_set::none;
        }

        impl.signals_ = reg->next_in_set;

        if (registrations_[signal_number] == reg)
            registrations_[signal_number] = reg->next_in_table;
        if (reg->prev_in_table)
            reg->prev_in_table->next_in_table = reg->next_in_table;
        if (reg->next_in_table)
            reg->next_in_table->prev_in_table = reg->prev_in_table;

        --state->registration_count[signal_number];
        --registration_count_[signal_number];

        delete reg;
    }

    if (first_error)
        return first_error;
    return {};
}

inline void
posix_signal_service::cancel_wait(posix_signal& impl)
{
    bool was_waiting = false;
    signal_op* op    = nullptr;

    {
        std::lock_guard lock(mutex_);
        impl.cancelled_ = true;
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
        op->cont_op.cont.h = op->h;
        op->d.post(op->cont_op.cont);
        sched_->work_finished();
    }
}

inline void
posix_signal_service::start_wait(posix_signal& impl, signal_op* op)
{
    {
        std::lock_guard lock(mutex_);

        // Check if cancel() was called before this wait started
        if (impl.cancelled_)
        {
            impl.cancelled_ = false;
            if (op->ec_out)
                *op->ec_out = make_error_code(capy::error::canceled);
            if (op->signal_out)
                *op->signal_out = 0;
            op->cont_op.cont.h = op->h;
            op->d.post(op->cont_op.cont);
            return;
        }

        // Check for queued signals first (signal arrived before wait started)
        signal_registration* reg = impl.signals_;
        while (reg)
        {
            if (reg->undelivered > 0)
            {
                --reg->undelivered;
                op->signal_number = reg->signal_number;
                // svc=nullptr: no work_finished needed since we never called work_started
                op->svc = nullptr;
                sched_->post(op);
                return;
            }
            reg = reg->next_in_set;
        }

        // No queued signals - wait for delivery
        impl.waiting_ = true;
        // svc=this: signal_op::operator() will call work_finished() to balance this
        op->svc = this;
        sched_->work_started();
    }
}

inline void
posix_signal_service::deliver_signal(int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return;

    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard lock(state->mutex);

    posix_signal_service* service = state->service_list;
    while (service)
    {
        std::lock_guard svc_lock(service->mutex_);

        signal_registration* reg = service->registrations_[signal_number];
        while (reg)
        {
            posix_signal* impl = static_cast<posix_signal*>(reg->owner);

            if (impl->waiting_)
            {
                impl->waiting_                  = false;
                impl->pending_op_.signal_number = signal_number;
                service->post(&impl->pending_op_);
            }
            else
            {
                ++reg->undelivered;
            }

            reg = reg->next_in_table;
        }

        service = service->next_;
    }
}

inline void
posix_signal_service::work_started() noexcept
{
    sched_->work_started();
}

inline void
posix_signal_service::work_finished() noexcept
{
    sched_->work_finished();
}

inline void
posix_signal_service::post(signal_op* op)
{
    sched_->post(op);
}

inline void
posix_signal_service::add_service(posix_signal_service* service)
{
    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard lock(state->mutex);

    service->next_ = state->service_list;
    service->prev_ = nullptr;
    if (state->service_list)
        state->service_list->prev_ = service;
    state->service_list = service;
}

inline void
posix_signal_service::remove_service(posix_signal_service* service)
{
    posix_signal_detail::signal_state* state =
        posix_signal_detail::get_signal_state();
    std::lock_guard lock(state->mutex);

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

// get_signal_service - factory function

inline posix_signal_service&
get_signal_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_signal_service>(sched);
}

} // namespace detail
} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_SERVICE_HPP
