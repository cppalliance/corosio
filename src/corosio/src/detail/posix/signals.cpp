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

// Include the appropriate scheduler based on backend
#if defined(BOOST_COROSIO_BACKEND_EPOLL)
#include "src/detail/epoll/scheduler.hpp"
#elif defined(BOOST_COROSIO_BACKEND_KQUEUE)
#include "src/detail/kqueue/scheduler.hpp"
#endif

#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/capy/error.hpp>
#include <mutex>

#include <signal.h>

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
    posix_signals* service_list = nullptr;
    std::size_t registration_count[max_signal_number] = {};
};

signal_state* get_signal_state()
{
    static signal_state state;
    return &state;
}

// C signal handler - must be async-signal-safe
extern "C" void corosio_posix_signal_handler(int signal_number)
{
    posix_signals::deliver_signal(signal_number);

    // Re-register handler (some systems reset to SIG_DFL after each signal)
    ::signal(signal_number, corosio_posix_signal_handler);
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
//
// posix_signal_impl
//
//------------------------------------------------------------------------------

posix_signal_impl::
posix_signal_impl(posix_signals& svc) noexcept
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
    capy::stop_token token,
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
//
// posix_signals
//
//------------------------------------------------------------------------------

posix_signals::
posix_signals(capy::execution_context& ctx)
    : sched_(ctx.use_service<posix_scheduler>())
{
    for (int i = 0; i < max_signal_number; ++i)
    {
        registrations_[i] = nullptr;
        registration_count_[i] = 0;
    }
    add_service(this);
}

posix_signals::
~posix_signals()
{
    remove_service(this);
}

void
posix_signals::
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

posix_signal_impl&
posix_signals::
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
posix_signals::
destroy_impl(posix_signal_impl& impl)
{
    {
        std::lock_guard lock(mutex_);
        impl_list_.remove(&impl);
    }

    delete &impl;
}

system::result<void>
posix_signals::
add_signal(
    posix_signal_impl& impl,
    int signal_number,
    signal_set::flags_t flags)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return make_error_code(system::errc::invalid_argument);

#if defined(BOOST_COROSIO_BACKEND_KQUEUE)
    // macOS (kqueue) only supports none and dont_care flags
    // since we use C signal() instead of sigaction()
    constexpr auto supported = signal_set::none | signal_set::dont_care;
    if ((flags & ~supported) != signal_set::none)
        return make_error_code(system::errc::operation_not_supported);
#endif

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

    if (reg && reg->signal_number == signal_number)
        return {};  // Already registered

    // Check for flag conflicts with existing registrations
    signal_registration* existing = registrations_[signal_number];
    if (existing)
    {
        signal_set::flags_t existing_flags = existing->flags;
        bool this_dont_care = (flags & signal_set::dont_care) != signal_set::none;
        bool existing_dont_care = (existing_flags & signal_set::dont_care) != signal_set::none;

        // If neither uses dont_care, flags must match
        if (!this_dont_care && !existing_dont_care)
        {
            // Compare actual flags (excluding dont_care bit)
            auto this_actual = flags & ~signal_set::dont_care;
            auto existing_actual = existing_flags & ~signal_set::dont_care;
            if (this_actual != existing_actual)
                return make_error_code(system::errc::invalid_argument);
        }
    }

    auto* new_reg = new signal_registration;
    new_reg->signal_number = signal_number;
    new_reg->flags = flags;
    new_reg->owner = &impl;
    new_reg->undelivered = 0;

    // Install signal handler on first global registration
    if (state->registration_count[signal_number] == 0)
    {
        if (::signal(signal_number, corosio_posix_signal_handler) == SIG_ERR)
        {
            delete new_reg;
            return make_error_code(system::errc::invalid_argument);
        }
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
posix_signals::
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
        if (::signal(signal_number, SIG_DFL) == SIG_ERR)
            return make_error_code(system::errc::invalid_argument);
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
posix_signals::
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
            if (::signal(signal_number, SIG_DFL) == SIG_ERR && !first_error)
                first_error = make_error_code(system::errc::invalid_argument);
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
posix_signals::
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
        sched_.on_work_finished();
    }
}

void
posix_signals::
start_wait(posix_signal_impl& impl, signal_op* op)
{
    {
        std::lock_guard lock(mutex_);

        signal_registration* reg = impl.signals_;
        while (reg)
        {
            if (reg->undelivered > 0)
            {
                --reg->undelivered;
                op->signal_number = reg->signal_number;
                op->svc = nullptr;
                sched_.post(op);
                return;
            }
            reg = reg->next_in_set;
        }

        // No queued signals - wait for delivery.
        // svc is set so signal_op::operator() calls work_finished().
        impl.waiting_ = true;
        op->svc = this;
        sched_.on_work_started();
    }
}

void
posix_signals::
deliver_signal(int signal_number)
{
    if (signal_number < 0 || signal_number >= max_signal_number)
        return;

    signal_state* state = get_signal_state();
    std::lock_guard lock(state->mutex);

    posix_signals* service = state->service_list;
    while (service)
    {
        std::lock_guard svc_lock(service->mutex_);

        signal_registration* reg = service->registrations_[signal_number];
        while (reg)
        {
            posix_signal_impl* impl = reg->owner;

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
posix_signals::
work_started() noexcept
{
    sched_.work_started();
}

void
posix_signals::
work_finished() noexcept
{
    sched_.work_finished();
}

void
posix_signals::
post(signal_op* op)
{
    sched_.post(op);
}

void
posix_signals::
add_service(posix_signals* service)
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
posix_signals::
remove_service(posix_signals* service)
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

} // namespace detail

//------------------------------------------------------------------------------
//
// signal_set implementation
//
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
    impl_ = &ctx.use_service<detail::posix_signals>().create_impl();
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

} // namespace corosio
} // namespace boost

#endif // !_WIN32
