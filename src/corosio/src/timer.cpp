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

// Not inline: wait_awaitable::await_suspend (defined in timer.hpp) calls
// this from translation units that may never include timer_service.hpp,
// so this must be the one strong definition the linker can always find
// wherever a detail::timer is used (every such user also needs timer's
// constructors, defined in this same translation unit).
std::coroutine_handle<>
timer::implementation::wait(waiter_node& w)
{
    // Already-expired fast path — no publication, no mutex.
    // Post instead of dispatch so the coroutine yields to the
    // scheduler, allowing other queued work to run.
    if (already_expired())
    {
        w.ec_ = {};
        w.d_.post(w.cont_);
        return std::noop_coroutine();
    }
    return publish(w);
}

std::coroutine_handle<>
timer::implementation::publish(waiter_node& w)
{
    // Publication-last invariant: fully initialize the waiter, count
    // its work, and arm cancellation BEFORE insert_waiter() publishes
    // it into the heap/waiter slot where a concurrent run() thread can fire
    // it. impl_ stays null until insert_waiter() sets it under the
    // mutex, so a stop callback that fires early (cancel_waiter) sees a
    // null impl_ and is a safe no-op. To avoid losing such an early
    // cancel, insert_waiter() re-checks stop_requested() under the lock
    // and completes as canceled if it fires in this window.
    w.impl_ = nullptr;
    w.svc_  = svc_;

    might_have_pending_waits_.store(true, std::memory_order_relaxed);
    svc_->get_scheduler().work_started();

    if (w.token_->stop_possible())
        w.arm_stop_cb();

    svc_->insert_waiter(*this, &w);

    return std::noop_coroutine();
}

std::coroutine_handle<>
timer::publish_wait(waiter_node& w)
{
    return get().publish(w);
}

bool
timer::rearm_wait(waiter_node& w, duration d) noexcept
{
    // The single waiter was popped before its op ran, so the impl is
    // out of the heap with no published waiters: expires_after only
    // stores the saturated expiry, and writing it is race-free.
    expires_after(d);
    auto& impl = get();
    // The drain that popped the waiter cleared the flag.
    impl.might_have_pending_waits_.store(true, std::memory_order_relaxed);
    try
    {
        impl.svc_->insert_waiter(impl, &w);
    }
    catch(std::bad_alloc const&)
    {
        // insert_waiter grows the heap before publishing anything,
        // so the waiter is untouched and the caller can complete
        // the wait through the normal resume path.
        return false;
    }
    return true;
}

// completion_op and canceller definitions live here, non-inline, for
// the same reason wait() does: the inline waiter_node constructor in
// timer.hpp references do_complete and the vtable from translation
// units that never include timer_service.hpp.

void
waiter_node::canceller::operator()() const
{
    waiter_->svc_->cancel_waiter(waiter_);
}

void
waiter_node::completion_op::do_complete(
    [[maybe_unused]] void* owner,
    scheduler_op* base,
    std::uint32_t,
    std::uint32_t)
{
    // owner is always non-null here. The destroy path (owner == nullptr)
    // is unreachable because completion_op overrides destroy() directly,
    // bypassing scheduler_op::destroy() which would call func_(nullptr, ...).
    BOOST_COROSIO_ASSERT(owner);
    static_cast<completion_op*>(base)->operator()();
}

void
waiter_node::completion_op::operator()()
{
    // The node lives in the resuming coroutine's frame: posting the
    // continuation is the last access, since the frame (and node)
    // may complete and die on another thread immediately after.
    auto* w = waiter_;
    // A true return means the waiter re-published itself: the frame
    // stays suspended, the wait's work count stays live, and the
    // node may already be firing on another thread — no access past
    // this point.
    if (w->on_fire_ && w->on_fire_(w->on_fire_ctx_))
        return;
    w->reset_stop_cb();
    auto d      = w->d_;
    auto& sched = w->svc_->get_scheduler();
    d.post(w->cont_);
    sched.work_finished();
}

void
waiter_node::completion_op::destroy()
{
    // Called during scheduler shutdown drain when this completion_op is
    // in the scheduler's ready queue (posted by cancel_timer() or
    // process_expired()). Balances the work_started() from
    // implementation::wait(), keeping the run-loop counter sane; no
    // shutdown path waits on that counter.
    //
    // This override also prevents scheduler_op::destroy() from calling
    // do_complete(nullptr, ...). See also: timer_service::shutdown()
    // which drains waiters still in the timer heap (the other path).
    // Destroying the frame also ends the node's storage, so it is
    // the last access.
    auto* w = waiter_;
    w->reset_stop_cb();
    auto h      = std::exchange(w->h_, {});
    auto& sched = w->svc_->get_scheduler();
    sched.work_finished();
    if (h)
        h.destroy();
}

} // namespace boost::corosio::detail
