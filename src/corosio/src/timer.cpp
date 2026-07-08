//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/timer.hpp>
#include <boost/corosio/detail/timer_service.hpp>

namespace boost::corosio::detail {

timer::~timer() = default;

timer::timer(capy::execution_context& ctx)
    : io_object(create_handle<detail::timer_service>(ctx))
{
}

timer::timer(capy::execution_context& ctx, time_point t) : timer(ctx)
{
    expires_at(t);
}

timer::timer(timer&& other) noexcept : io_object(std::move(other)) {}

timer&
timer::operator=(timer&& other) noexcept
{
    if (this != &other)
        h_ = std::move(other.h_);
    return *this;
}

std::size_t
timer::do_cancel()
{
    return detail::timer_service_cancel(get());
}

std::size_t
timer::do_cancel_one()
{
    return detail::timer_service_cancel_one(get());
}

std::size_t
timer::do_update_expiry()
{
    return detail::timer_service_update_expiry(get());
}

// Not inline: wait_awaitable::await_suspend (defined in timer.hpp) calls
// this from translation units that may never include timer_service.hpp,
// so this must be the one strong definition the linker can always find
// wherever a detail::timer is used (every such user also needs timer's
// constructors, defined in this same translation unit).
std::coroutine_handle<>
timer::implementation::wait(
    std::coroutine_handle<> h,
    capy::executor_ref d,
    std::stop_token token,
    std::error_code* ec,
    capy::continuation* cont)
{
    // Already-expired fast path — no waiter_node, no mutex.
    // Post instead of dispatch so the coroutine yields to the
    // scheduler, allowing other queued work to run.
    if (heap_index_.load(std::memory_order_relaxed) == npos)
    {
        if (expiry_ == (time_point::min)() || expiry_ <= clock_type::now())
        {
            if (ec)
                *ec = {};
            d.post(*cont);
            return std::noop_coroutine();
        }
    }

    // Publication-last invariant: fully initialize the waiter, count
    // its work, and arm cancellation BEFORE insert_waiter() publishes
    // it into the heap/list where a concurrent run() thread can fire
    // it. impl_ stays null until insert_waiter() sets it under the
    // mutex, so a stop callback that fires early (cancel_waiter) sees a
    // null impl_ and is a safe no-op. To avoid losing such an early
    // cancel, insert_waiter() re-checks stop_requested() under the lock
    // and completes as canceled if it fires in this window.
    auto* w    = svc_->create_waiter();
    w->impl_   = nullptr;
    w->svc_    = svc_;
    w->h_      = h;
    w->cont_   = cont;
    w->d_      = d;
    w->token_  = std::move(token);
    w->ec_out_ = ec;

    might_have_pending_waits_.store(true, std::memory_order_relaxed);
    svc_->get_scheduler().work_started();

    if (w->token_.stop_possible())
        w->stop_cb_.emplace(w->token_, waiter_node::canceller{w});

    svc_->insert_waiter(*this, w);

    return std::noop_coroutine();
}

} // namespace boost::corosio::detail
