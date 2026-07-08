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
#include <boost/capy/error.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/io_result.hpp>

#include <chrono>
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>

namespace boost::corosio {

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

        // A non-io_context executor cannot supply a timer service,
        // and await_suspend is driven through a noexcept wrapper, so
        // translate the service-lookup failure into a clear terminate.
        try
        {
            timer_.emplace(env->executor.context());
        }
        catch(std::logic_error const&)
        {
            detail::throw_logic_error(
                "delay requires an io_context-backed executor");
        }
        catch(std::exception const& e)
        {
            detail::throw_logic_error(e.what());
        }

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
    using namespace std::chrono;
    // Narrow reps wrap if nanoseconds::max() is converted into them;
    // a double comparison clamps safely in both directions.
    using dsec = duration<double>;
    auto ns = dsec(dur) >= dsec((nanoseconds::max)())
        ? (nanoseconds::max)()
        : dsec(dur) <= dsec((nanoseconds::min)())
            ? (nanoseconds::min)()
            : duration_cast<nanoseconds>(dur);
    return delay_awaitable(ns);
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

} // namespace boost::corosio

#endif
