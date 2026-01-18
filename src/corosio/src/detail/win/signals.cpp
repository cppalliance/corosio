//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifdef _WIN32

#include "src/detail/win/signals.hpp"
#include "src/detail/win/scheduler.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/error.hpp>

#include <csignal>
#include <mutex>

namespace boost {
namespace corosio {
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

// C signal handler - must be async-signal-safe
extern "C" void corosio_signal_handler(int signal_number)
{
    win_signals::deliver_signal(signal_number);

    // Re-register handler (Windows resets to SIG_DFL after each signal)
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

    // Capture svc before resuming (coro may destroy us)
    auto* service = svc;
    svc = nullptr;

    d.dispatch(h).resume();

    // Balance the on_work_started() from start_wait
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
    capy::any_executor_ref d,
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

system::error_code
win_signal_impl::
add(int signal_number)
{
    return svc_.add_signal(*this, signal_number);
}

system::error_code
win_signal_impl::
remove(int signal_number)
{
    return svc_.remove_signal(*this, signal_number);
}

system::error_code
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

system::error_code
win_signals::
add_signal(
    win_signal_impl& impl,
    int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

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

system::error_code
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

system::error_code
win_signals::
clear_signals(win_signal_impl& impl)
{
    signal_state* state = get_signal_state();
    std::lock_guard<std::mutex> state_lock(state->mutex);
    std::lock_guard<win_mutex> lock(mutex_);

    while (signal_registration* reg = impl.signals_)
    {
        int signal_number = reg->signal_number;

        // Restore default handler if last registration
        if (state->registration_count[signal_number] == 1)
            ::signal(signal_number, SIG_DFL);

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

    // Deliver to all services
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
                // Queue for later
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
signal_set(capy::execution_context& ctx, int signal_number_1)
    : io_object(ctx)
{
    impl_ = &ctx.use_service<detail::win_signals>().create_impl();
    add(signal_number_1);
}

signal_set::
signal_set(
    capy::execution_context& ctx,
    int signal_number_1,
    int signal_number_2)
    : io_object(ctx)
{
    impl_ = &ctx.use_service<detail::win_signals>().create_impl();
    add(signal_number_1);
    add(signal_number_2);
}

signal_set::
signal_set(
    capy::execution_context& ctx,
    int signal_number_1,
    int signal_number_2,
    int signal_number_3)
    : io_object(ctx)
{
    impl_ = &ctx.use_service<detail::win_signals>().create_impl();
    add(signal_number_1);
    add(signal_number_2);
    add(signal_number_3);
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

void
signal_set::
add(int signal_number)
{
    system::error_code ec = get().add(signal_number);
    if (ec)
        detail::throw_system_error(ec, "signal_set::add");
}

void
signal_set::
add(int signal_number, system::error_code& ec)
{
    ec = get().add(signal_number);
}

void
signal_set::
remove(int signal_number)
{
    system::error_code ec = get().remove(signal_number);
    if (ec)
        detail::throw_system_error(ec, "signal_set::remove");
}

void
signal_set::
remove(int signal_number, system::error_code& ec)
{
    ec = get().remove(signal_number);
}

void
signal_set::
clear()
{
    system::error_code ec = get().clear();
    if (ec)
        detail::throw_system_error(ec, "signal_set::clear");
}

void
signal_set::
clear(system::error_code& ec)
{
    ec = get().clear();
}

void
signal_set::
cancel()
{
    get().cancel();
}

} // namespace corosio
} // namespace boost

#endif // _WIN32
