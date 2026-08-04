//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DELAY_HPP
#define BOOST_COROSIO_DELAY_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/timer.hpp>
#include <boost/corosio/wait_traits.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/io_result.hpp>

#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace boost::corosio {

namespace detail {

// Narrow reps wrap if nanoseconds::max() is converted into them;
// a double comparison clamps safely in both directions.
template<typename Rep, typename Period>
std::chrono::nanoseconds
clamp_to_ns(std::chrono::duration<Rep, Period> dur) noexcept
{
    using namespace std::chrono;
    using dsec = duration<double>;
    if constexpr (std::is_floating_point_v<Rep>)
    {
        // NaN fails both clamp comparisons and would reach the
        // cast; treat it as no wait rather than undefined behavior.
        if (dur != dur)
            return nanoseconds::zero();
    }
    return dsec(dur) >= dsec((nanoseconds::max)())
        ? (nanoseconds::max)()
        : dsec(dur) <= dsec((nanoseconds::min)())
            ? (nanoseconds::min)()
            : duration_cast<nanoseconds>(dur);
}

// A non-io_context executor cannot supply a timer service, and
// await_suspend is driven through a noexcept wrapper, so translate
// the service-lookup failure into a clear terminate.
inline void
emplace_delay_timer(
    std::optional<timer>& t, capy::execution_context& ctx)
{
    try
    {
        t.emplace(ctx);
    }
    catch(std::logic_error const&)
    {
        throw_logic_error(
            "delay requires an io_context-backed executor");
    }
    catch(std::exception const& e)
    {
        throw_logic_error(e.what());
    }
}

} // namespace detail

/** IoAwaitable returned by @ref delay.

    Suspends the calling coroutine until the deadline elapses or
    the environment's stop token is activated, whichever comes
    first. A deadline already elapsed at suspension, or a stop
    token already active, resumes the coroutine inline, without
    starting a timer (see Cancellation below). Otherwise the
    coroutine resumes through the executor once the timer fires
    or a mid-wait cancellation arrives.

    Not intended to be named directly; use the @ref delay factory
    overloads instead.

    @par Preconditions
    The awaiting coroutine's executor must belong to an
    `io_context`. Any other execution context terminates with a
    diagnostic, because silently running without a timer would
    drop the requested delay.

    @par Cancellation
    If stop is already requested before suspension, the coroutine
    resumes immediately with `error::canceled`. If stop is
    requested while suspended, the pending wait is cancelled and
    the coroutine resumes with `error::canceled`. Requesting stop
    from another thread while the io_context runs in
    single_threaded mode (auto-enabled at concurrency_hint == 1)
    is not permitted by io_context's threading rules;
    cross-thread cancellation requires a multi-threaded-capable
    context.

    @see delay
*/
class delay_awaitable
{
    // wait() names timer's private awaitable type; decltype is
    // the only way to store it here.
    using wait_type = decltype(std::declval<detail::timer&>().wait());

    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::nanoseconds dur_{};
    bool has_deadline_ = false;
    bool canceled_ = false;
    std::optional<detail::timer> timer_;
    std::optional<wait_type> wait_;

public:
    /// Construct an awaitable that waits for `dur` nanoseconds.
    explicit delay_awaitable(std::chrono::nanoseconds dur) noexcept
        : dur_(dur)
    {
    }

    /// Construct an awaitable that waits until `tp`.
    explicit delay_awaitable(
        std::chrono::steady_clock::time_point tp) noexcept
        : deadline_(tp)
        , has_deadline_(true)
    {
    }

    /// Construct by transferring state from `other`.
    // Only moved before await_suspend; wait_ is engaged after.
    delay_awaitable(delay_awaitable&&) = default;

    delay_awaitable(delay_awaitable const&) = delete;
    delay_awaitable& operator=(delay_awaitable const&) = delete;
    delay_awaitable& operator=(delay_awaitable&&) = delete;

    /// Return false unconditionally; see await_suspend.
    // The elapsed-deadline fast path must run after the stop-token
    // check, and only await_suspend receives the env carrying it.
    bool await_ready() const noexcept
    {
        return false;
    }

    /// Resume inline if stopped or elapsed; else wait on a timer.
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
    {
        if(env->stop_token.stop_requested())
        {
            canceled_ = true;
            return h;
        }

        // Elapsed deadlines complete synchronously, but only once a
        // pending stop request has already been ruled out above.
        if(has_deadline_ ?
            deadline_ <= std::chrono::steady_clock::now() :
            dur_.count() <= 0)
            return h;

        detail::emplace_delay_timer(timer_, env->executor.context());

        if(has_deadline_)
            timer_->expires_at(deadline_);
        else
            timer_->expires_after(dur_);

        wait_.emplace(timer_->wait());
        return wait_->await_suspend(h, env);
    }

    /// Return empty on expiry, `error::canceled` if stop won.
    capy::io_result<> await_resume() noexcept
    {
        if(canceled_)
            return {capy::error::canceled};
        if(wait_)
            return wait_->await_resume();
        return {};
    }
};

/** IoAwaitable returned by the clock overloads of @ref delay.

    Suspends the calling coroutine until `Clock::now()` reaches the
    deadline or the environment's stop token is activated. The wait
    is a sequence of steady-clock timer waits: after each expiry the
    clock is re-read and, if the deadline is unreached, the same
    frame-embedded waiter is re-published for the next
    `Traits::to_wait_duration` cap — without resuming the coroutine
    and without allocating.

    Not intended to be named directly; use the @ref delay factory
    overloads instead.

    @par Preconditions
    The awaiting coroutine's executor must belong to an
    `io_context`. Any other execution context terminates with a
    diagnostic, because silently running without a timer would
    drop the requested delay.

    @par Cancellation
    Identical to @ref delay_awaitable: stop already requested
    resumes inline with `error::canceled`; stop while suspended
    cancels the pending wait, including between re-arms.

    @see delay, wait_traits
*/
template<class Clock, class Traits>
class clock_delay_awaitable
{
    typename Clock::time_point deadline_{};
    bool canceled_ = false;
    std::optional<detail::timer> timer_;
    detail::waiter_node w_;

    std::chrono::nanoseconds
    next_wait(typename Clock::time_point now) const noexcept
    {
        return detail::clamp_to_ns(
            Traits::to_wait_duration(deadline_ - now));
    }

    // Runs on the scheduler thread executing the completion op,
    // before the continuation is posted, so the frame cannot die
    // concurrently.
    static bool on_fire(void* ctx) noexcept
    {
        auto* self = static_cast<clock_delay_awaitable*>(ctx);
        // Canceled: resume and surface the error
        if(self->w_.ec_)
            return false;
        auto now = Clock::now();
        if(now >= self->deadline_)
            return false;
        // Re-publish and return without touching the node again:
        // the wait may complete on another thread immediately after.
        if(self->timer_->rearm_wait(self->w_, self->next_wait(now)))
            return true;
        // Heap growth failed; finish the wait with an error rather
        // than strand the frame with an unbalanced work count.
        self->w_.ec_ = std::make_error_code(std::errc::not_enough_memory);
        return false;
    }

public:
    /// Construct an awaitable that waits until `tp` on `Clock`.
    explicit clock_delay_awaitable(
        typename Clock::time_point tp) noexcept
        : deadline_(tp)
    {
    }

    /// Construct by transferring the deadline from `other`.
    // Only moved before await_suspend; w_ is quiescent until then.
    clock_delay_awaitable(clock_delay_awaitable&& other) noexcept
        : deadline_(other.deadline_)
    {
    }

    clock_delay_awaitable(clock_delay_awaitable const&) = delete;
    clock_delay_awaitable&
    operator=(clock_delay_awaitable const&) = delete;
    clock_delay_awaitable&
    operator=(clock_delay_awaitable&&) = delete;

    /// Return false unconditionally; see await_suspend.
    // The elapsed-deadline fast path must run after the stop-token
    // check, and only await_suspend receives the env carrying it.
    bool await_ready() const noexcept
    {
        return false;
    }

    /// Resume inline if stopped or reached; else wait on a timer.
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
    {
        if(env->stop_token.stop_requested())
        {
            canceled_ = true;
            return h;
        }

        auto now = Clock::now();
        if(now >= deadline_)
            return h;

        detail::emplace_delay_timer(timer_, env->executor.context());

        timer_->expires_after(next_wait(now));

        w_.bind(h, *env);
        w_.on_fire_     = &on_fire;
        w_.on_fire_ctx_ = this;
        // Never the elapsed fast path: a capped expiry that elapses
        // before publication must still reach on_fire, not complete
        // the clock wait early.
        return timer_->publish_wait(w_);
    }

    /// Return empty on deadline, `error::canceled` if stop won.
    capy::io_result<> await_resume() noexcept
    {
        if(canceled_)
            return {capy::error::canceled};
        if(timer_)
            return {w_.ec_};
        return {};
    }
};

/** Suspend the current coroutine for a duration.

    Returns an IoAwaitable that completes at or after the
    specified duration, or earlier if the environment's stop
    token is activated. Zero or negative durations complete
    synchronously.

    @par Example
    @code
    auto [ec] = co_await delay(std::chrono::milliseconds(100));
    @endcode

    @param dur The duration to wait.

    @return A @ref delay_awaitable yielding `io_result<>`.
*/
template<typename Rep, typename Period>
[[nodiscard]] delay_awaitable
delay(std::chrono::duration<Rep, Period> dur) noexcept
{
    return delay_awaitable(detail::clamp_to_ns(dur));
}

/** Suspend the current coroutine until a time point.

    Returns an IoAwaitable that completes at or after `tp`, or
    earlier if the environment's stop token is activated. Time
    points already reached complete synchronously.

    @param tp The steady-clock time point to wait until.

    @return A @ref delay_awaitable yielding `io_result<>`.
*/
[[nodiscard]] inline delay_awaitable
delay(std::chrono::steady_clock::time_point tp) noexcept
{
    return delay_awaitable(tp);
}

/** Suspend the current coroutine until a time point on `Clock`.

    Returns an IoAwaitable that completes at or after the first
    observation of `Clock::now() >= tp`, or earlier if the
    environment's stop token is activated. The wait is one or more
    bounded steady-clock waits, re-reading `Clock::now()` after
    each; `Traits::to_wait_duration` bounds each one. With the
    default @ref wait_traits a single full-length wait is used, so
    an adjustment of `Clock` mid-wait is observed only at natural
    wakeup; supply capping traits to bound that latency. Time
    points already reached complete synchronously.

    @note `Clock::now()` and `Traits::to_wait_duration` are invoked
    on the io_context's run thread and must not throw or block.

    @par Example
    @code
    auto [ec] = co_await delay(
        std::chrono::system_clock::now() + std::chrono::minutes(5));
    @endcode

    @tparam Traits The wait-traits policy; `void` selects
        @ref wait_traits.

    @param tp The time point to wait until.

    @return A @ref clock_delay_awaitable yielding `io_result<>`.
*/
template<class Traits = void, class Clock, class Duration>
    requires (!std::same_as<Clock, std::chrono::steady_clock>) &&
        (std::is_void_v<Traits> || WaitTraits<Traits, Clock>)
[[nodiscard]] auto
delay(std::chrono::time_point<Clock, Duration> tp) noexcept
{
    using traits_type = std::conditional_t<
        std::is_void_v<Traits>, wait_traits<Clock>, Traits>;
    // ceil preserves completes-at-or-after when Duration is coarser
    // than the clock's native duration
    return clock_delay_awaitable<Clock, traits_type>(
        std::chrono::ceil<typename Clock::duration>(tp));
}

} // namespace boost::corosio

#endif
