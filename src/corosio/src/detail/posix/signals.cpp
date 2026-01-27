//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_SIGNAL_POSIX)

#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/error.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include "src/detail/intrusive.hpp"
#include "src/detail/scheduler_op.hpp"

#include <coroutine>
#include <cstddef>
#include <mutex>
#include <stop_token>

#include <signal.h>

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

    2. posix_signals_impl (one per execution_context)
       - Maintains registrations_[] table indexed by signal number
       - Each slot is a doubly-linked list of signal_registrations for that signal
       - Also maintains impl_list_ of all posix_signal_impl objects it owns

    3. posix_signal_impl (one per signal_set)
       - Owns a singly-linked list (sorted by signal number) of signal_registrations
       - Contains the pending_op_ used for async_wait operations

    Signal Delivery Flow
    --------------------

    1. Signal arrives -> corosio_posix_signal_handler() (must be async-signal-safe)
       -> deliver_signal()

    2. deliver_signal() iterates all posix_signals_impl services:
       - If a signal_set is waiting (impl->waiting_ == true), post the signal_op
         to the scheduler for immediate completion
       - Otherwise, increment reg->undelivered to queue the signal

    3. When async_wait() is called via start_wait():
       - First check for queued signals (undelivered > 0); if found, post
         immediate completion without blocking
       - Otherwise, set waiting_ = true and call on_work_started() to keep
         the io_context alive

    Locking Protocol
    ----------------

    Two mutex levels exist (MUST acquire in this order to avoid deadlock):
      1. signal_state::mutex - protects handler registration and service list
      2. posix_signals_impl::mutex_ - protects per-service registration tables

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
      - start_wait() calls sched_->on_work_started() to prevent io_context::run()
        from returning while we wait
      - signal_op::svc is set to point to the service
      - signal_op::operator()() calls work_finished() after resuming the coroutine

    If a signal was already queued (undelivered > 0), no work tracking is needed
    because completion is posted immediately.
*/

namespace boost::corosio {

namespace detail {

// Forward declarations
class posix_signals_impl;

// Maximum signal number supported (NSIG is typically 64 on Linux)
enum { max_signal_number = 64 };

//------------------------------------------------------------------------------
// signal_op - pending async_wait operation
//------------------------------------------------------------------------------

struct signal_op : scheduler_op
{
    capy::coro h;
    capy::executor_ref d;
    system::error_code* ec_out = nullptr;
    int* signal_out = nullptr;
    int signal_number = 0;
    posix_signals_impl* svc = nullptr;  // For work_finished callback

    void operator()() override;
    void destroy() override;
};

//------------------------------------------------------------------------------
// signal_registration - per-signal registration tracking
//------------------------------------------------------------------------------

struct signal_registration
{
    int signal_number = 0;
    signal_set::flags_t flags = signal_set::none;
    signal_set::signal_set_impl* owner = nullptr;
    std::size_t undelivered = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set = nullptr;
};

//------------------------------------------------------------------------------
// posix_signal_impl - per-signal_set implementation
//------------------------------------------------------------------------------

class posix_signal_impl
    : public signal_set::signal_set_impl
    , public intrusive_list<posix_signal_impl>::node
{
    friend class posix_signals_impl;

    posix_signals_impl& svc_;
    signal_registration* signals_ = nullptr;
    signal_op pending_op_;
    bool waiting_ = false;

public:
    explicit posix_signal_impl(posix_signals_impl& svc) noexcept;

    void release() override;

    void wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        system::error_code*,
        int*) override;

    system::result<void> add(int signal_number, signal_set::flags_t flags) override;
    system::result<void> remove(int signal_number) override;
    system::result<void> clear() override;
    void cancel() override;
};

//------------------------------------------------------------------------------
// posix_signals_impl - concrete service implementation
//------------------------------------------------------------------------------

class posix_signals_impl : public posix_signals
{
public:
    using key_type = posix_signals;

    posix_signals_impl(capy::execution_context& ctx, scheduler& sched);
    ~posix_signals_impl();

    posix_signals_impl(posix_signals_impl const&) = delete;
    posix_signals_impl& operator=(posix_signals_impl const&) = delete;

    void shutdown() override;
    signal_set::signal_set_impl& create_impl() override;

    void destroy_impl(posix_signal_impl& impl);

    system::result<void> add_signal(
        posix_signal_impl& impl,
        int signal_number,
        signal_set::flags_t flags);

    system::result<void> remove_signal(
        posix_signal_impl& impl,
        int signal_number);

    system::result<void> clear_signals(posix_signal_impl& impl);

    void cancel_wait(posix_signal_impl& impl);
    void start_wait(posix_signal_impl& impl, signal_op* op);

    static void deliver_signal(int signal_number);

    void work_started() noexcept;
    void work_finished() noexcept;
    void post(signal_op* op);

private:
    static void add_service(posix_signals_impl* service);
    static void remove_service(posix_signals_impl* service);

    scheduler* sched_;
    std::mutex mutex_;
    intrusive_list<posix_signal_impl> impl_list_;

    // Per-signal registration table
    signal_registration* registrations_[max_signal_number];

    // Registration counts for each signal
    std::size_t registration_count_[max_signal_number];

    // Linked list of all posix_signals_impl services for signal delivery
    posix_signals_impl* next_ = nullptr;
    posix_signals_impl* prev_ = nullptr;
};

//------------------------------------------------------------------------------
// Global signal state
//------------------------------------------------------------------------------

namespace {

struct signal_state
{
    std::mutex mutex;
    posix_signals_impl* service_list = nullptr;
    std::size_t registration_count[max_signal_number] = {};
    signal_set::flags_t registered_flags[max_signal_number] = {};
};

signal_state* get_signal_state()
{
    static signal_state state;
    return &state;
}

// Check if requested flags are supported on this platform.
// Returns true if all flags are supported, false otherwise.
bool flags_supported(signal_set::flags_t flags)
{
#ifndef SA_NOCLDWAIT
    if (flags & signal_set::no_child_wait)
        return false;
#endif
    return true;
}

// Map abstract flags to sigaction() flags.
// Caller must ensure flags_supported() returns true first.
int to_sigaction_flags(signal_set::flags_t flags)
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
bool flags_compatible(
    signal_set::flags_t existing,
    signal_set::flags_t requested)
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
extern "C" void corosio_posix_signal_handler(int signal_number)
{
    posix_signals_impl::deliver_signal(signal_number);
    // Note: With sigaction(), the handler persists automatically
    // (unlike some signal() implementations that reset to SIG_DFL)
}

} // namespace

//------------------------------------------------------------------------------
// signal_op implementation
//------------------------------------------------------------------------------

void
signal_op::
operator()()
{
    if (ec_out)
        *ec_out = {};
    if (signal_out)
        *signal_out = signal_number;

    // Capture svc before resuming (coro may destroy us)
    auto* service = svc;
    svc = nullptr;

    d.post(h);

    // Balance the on_work_started() from start_wait
    if (service)
        service->work_finished();
}

void
signal_op::
destroy()
{
    // No-op: signal_op is embedded in posix_signal_impl
}

//------------------------------------------------------------------------------
// posix_signal_impl implementation
//------------------------------------------------------------------------------

posix_signal_impl::
posix_signal_impl(posix_signals_impl& svc) noexcept
    : svc_(svc)
{
}

void
posix_signal_impl::
release()
{
    clear();
    cancel();
    svc_.destroy_impl(*this);
}

void
posix_signal_impl::
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

    if (token.stop_requested())
    {
        if (ec)
            *ec = make_error_code(capy::error::canceled);
        if (signal_out)
            *signal_out = 0;
        d.post(h);
        return;
    }

    svc_.start_wait(*this, &pending_op_);
}

system::result<void>
posix_signal_impl::
add(int signal_number, signal_set::flags_t flags)
{
    return svc_.add_signal(*this, signal_number, flags);
}

system::result<void>
posix_signal_impl::
remove(int signal_number)
{
    return svc_.remove_signal(*this, signal_number);
}

system::result<void>
posix_signal_impl::
clear()
{
    return svc_.clear_signals(*this);
}

void
posix_signal_impl::
cancel()
{
    svc_.cancel_wait(*this);
}

//------------------------------------------------------------------------------
// posix_signals_impl implementation
//------------------------------------------------------------------------------

posix_signals_impl::
posix_signals_impl(capy::execution_context&, scheduler& sched)
    : sched_(&sched)
{
    for (int i = 0; i < max_signal_number; ++i)
    {
        registrations_[i] = nullptr;
        registration_count_[i] = 0;
    }
    add_service(this);
}

posix_signals_impl::
~posix_signals_impl()
{
    remove_service(this);
}

void
posix_signals_impl::
shutdown()
{
    std::lock_guard lock(mutex_);

    for (auto* impl = impl_list_.pop_front(); impl != nullptr;
         impl = impl_list_.pop_front())
    {
        while (auto* reg = impl->signals_)
        {
            impl->signals_ = reg->next_in_set;
            delete reg;
        }
        delete impl;
    }
}

signal_set::signal_set_impl&
posix_signals_impl::
create_impl()
{
    auto* impl = new posix_signal_impl(*this);

    {
        std::lock_guard lock(mutex_);
        impl_list_.push_back(impl);
    }

    return *impl;
}

void
posix_signals_impl::
destroy_impl(posix_signal_impl& impl)
{
    {
        std::lock_guard lock(mutex_);
        impl_list_.remove(&impl);
    }

    delete &impl;
}

system::result<void>
posix_signals_impl::
add_signal(
    posix_signal_impl& impl,
    int signal_number,
    signal_set::flags_t flags)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

    // Validate that requested flags are supported on this platform
    // (e.g., SA_NOCLDWAIT may not be available on all POSIX systems)
    if (!flags_supported(flags))
        return make_error_code(system::errc::operation_not_supported);

    signal_state* state = get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    // Find insertion point (list is sorted by signal number)
    signal_registration** insertion_point = &impl.signals_;
    signal_registration* reg = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        insertion_point = &reg->next_in_set;
        reg = reg->next_in_set;
    }

    // Already registered in this set - check flag compatibility
    // (same signal_set adding same signal twice with different flags)
    if (reg && reg->signal_number == signal_number)
    {
        if (!flags_compatible(reg->flags, flags))
            return make_error_code(system::errc::invalid_argument);
        return {};
    }

    // Check flag compatibility with global registration
    // (different signal_set already registered this signal with different flags)
    if (state->registration_count[signal_number] > 0)
    {
        if (!flags_compatible(state->registered_flags[signal_number], flags))
            return make_error_code(system::errc::invalid_argument);
    }

    auto* new_reg = new signal_registration;
    new_reg->signal_number = signal_number;
    new_reg->flags = flags;
    new_reg->owner = &impl;
    new_reg->undelivered = 0;

    // Install signal handler on first global registration
    if (state->registration_count[signal_number] == 0)
    {
        struct sigaction sa = {};
        sa.sa_handler = corosio_posix_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = to_sigaction_flags(flags);

        if (::sigaction(signal_number, &sa, nullptr) < 0)
        {
            delete new_reg;
            return make_error_code(system::errc::invalid_argument);
        }

        // Store the flags used for first registration
        state->registered_flags[signal_number] = flags;
    }

    new_reg->next_in_set = reg;
    *insertion_point = new_reg;

    new_reg->next_in_table = registrations_[signal_number];
    new_reg->prev_in_table = nullptr;
    if (registrations_[signal_number])
        registrations_[signal_number]->prev_in_table = new_reg;
    registrations_[signal_number] = new_reg;

    ++state->registration_count[signal_number];
    ++registration_count_[signal_number];

    return {};
}

system::result<void>
posix_signals_impl::
remove_signal(
    posix_signal_impl& impl,
    int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

    signal_state* state = get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    signal_registration** deletion_point = &impl.signals_;
    signal_registration* reg = impl.signals_;
    while (reg && reg->signal_number < signal_number)
    {
        deletion_point = &reg->next_in_set;
        reg = reg->next_in_set;
    }

    if (!reg || reg->signal_number != signal_number)
        return {};

    // Restore default handler on last global unregistration
    if (state->registration_count[signal_number] == 1)
    {
        struct sigaction sa = {};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        if (::sigaction(signal_number, &sa, nullptr) < 0)
            return make_error_code(system::errc::invalid_argument);

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

system::result<void>
posix_signals_impl::
clear_signals(posix_signal_impl& impl)
{
    signal_state* state = get_signal_state();
    std::lock_guard state_lock(state->mutex);
    std::lock_guard lock(mutex_);

    system::error_code first_error;

    while (signal_registration* reg = impl.signals_)
    {
        int signal_number = reg->signal_number;

        if (state->registration_count[signal_number] == 1)
        {
            struct sigaction sa = {};
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;

            if (::sigaction(signal_number, &sa, nullptr) < 0 && !first_error)
                first_error = make_error_code(system::errc::invalid_argument);

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

void
posix_signals_impl::
cancel_wait(posix_signal_impl& impl)
{
    bool was_waiting = false;
    signal_op* op = nullptr;

    {
        std::lock_guard lock(mutex_);
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
        op->d.post(op->h);
        sched_->on_work_finished();
    }
}

void
posix_signals_impl::
start_wait(posix_signal_impl& impl, signal_op* op)
{
    {
        std::lock_guard lock(mutex_);

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
        sched_->on_work_started();
    }
}

void
posix_signals_impl::
deliver_signal(int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return;

    signal_state* state = get_signal_state();
    std::lock_guard lock(state->mutex);

    posix_signals_impl* service = state->service_list;
    while (service)
    {
        std::lock_guard svc_lock(service->mutex_);

        signal_registration* reg = service->registrations_[signal_number];
        while (reg)
        {
            posix_signal_impl* impl = static_cast<posix_signal_impl*>(reg->owner);

            if (impl->waiting_)
            {
                impl->waiting_ = false;
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

void
posix_signals_impl::
work_started() noexcept
{
    sched_->work_started();
}

void
posix_signals_impl::
work_finished() noexcept
{
    sched_->work_finished();
}

void
posix_signals_impl::
post(signal_op* op)
{
    sched_->post(op);
}

void
posix_signals_impl::
add_service(posix_signals_impl* service)
{
    signal_state* state = get_signal_state();
    std::lock_guard lock(state->mutex);

    service->next_ = state->service_list;
    service->prev_ = nullptr;
    if (state->service_list)
        state->service_list->prev_ = service;
    state->service_list = service;
}

void
posix_signals_impl::
remove_service(posix_signals_impl* service)
{
    signal_state* state = get_signal_state();
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

//------------------------------------------------------------------------------
// get_signal_service - factory function
//------------------------------------------------------------------------------

posix_signals&
get_signal_service(capy::execution_context& ctx, scheduler& sched)
{
    return ctx.make_service<posix_signals_impl>(sched);
}

} // namespace detail

//------------------------------------------------------------------------------
// signal_set implementation
//------------------------------------------------------------------------------

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
    auto* svc = ctx.find_service<detail::posix_signals>();
    if (!svc)
        detail::throw_logic_error("signal_set: signal service not initialized");
    impl_ = &svc->create_impl();
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

#endif // BOOST_COROSIO_SIGNAL_POSIX
