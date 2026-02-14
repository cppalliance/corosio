//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TIMER_HPP
#define BOOST_COROSIO_TIMER_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/concept/executor.hpp>
#include <system_error>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <limits>
#include <stop_token>

namespace boost::corosio {

/** An asynchronous timer for coroutine I/O.

    This class provides asynchronous timer operations that return
    awaitable types. The timer can be used to schedule operations
    to occur after a specified duration or at a specific time point.

    Multiple coroutines may wait concurrently on the same timer.
    When the timer expires, all waiters complete with success. When
    the timer is cancelled, all waiters complete with an error that
    compares equal to `capy::cond::canceled`.

    Each timer operation participates in the affine awaitable protocol,
    ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Semantics
    Wraps platform timer facilities via the io_context reactor.
    Operations dispatch to OS timer APIs (timerfd, IOCP timers,
    kqueue EVFILT_TIMER).
*/
class BOOST_COROSIO_DECL timer : public io_object
{
    struct wait_awaitable
    {
        timer& t_;
        std::stop_token token_;
        mutable std::error_code ec_;

        explicit wait_awaitable(timer& t) noexcept : t_(t) {}

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {capy::error::canceled};
            return {ec_};
        }

        auto await_suspend(
            std::coroutine_handle<> h,
            capy::io_env const* env) -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            auto& impl = t_.get();
            // Inline fast path: already expired and not in the heap
            if (impl.heap_index_ == timer_impl::npos &&
                (impl.expiry_ == (time_point::min)() ||
                    impl.expiry_ <= clock_type::now()))
            {
                ec_ = {};
                auto d = env->executor;
                d.post(h);
                return std::noop_coroutine();
            }
            return impl.wait(
                h, env->executor, std::move(token_), &ec_);
        }
    };

public:
    struct timer_impl : io_object_impl
    {
        static constexpr std::size_t npos =
            (std::numeric_limits<std::size_t>::max)();

        std::chrono::steady_clock::time_point expiry_{};
        std::size_t heap_index_ = npos;
        bool might_have_pending_waits_ = false;

        virtual std::coroutine_handle<> wait(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            std::error_code*) = 0;
    };

public:
    /// The clock type used for time operations.
    using clock_type = std::chrono::steady_clock;

    /// The time point type for absolute expiry times.
    using time_point = clock_type::time_point;

    /// The duration type for relative expiry times.
    using duration = clock_type::duration;

    /** Destructor.

        Cancels any pending operations and releases timer resources.
    */
    ~timer();

    /** Construct a timer from an execution context.

        @param ctx The execution context that will own this timer.
    */
    explicit timer(capy::execution_context& ctx);

    /** Construct a timer with an initial absolute expiry time.

        @param ctx The execution context that will own this timer.
        @param t The initial expiry time point.
    */
    timer(capy::execution_context& ctx, time_point t);

    /** Construct a timer with an initial relative expiry time.

        @param ctx The execution context that will own this timer.
        @param d The initial expiry duration relative to now.
    */
    template<class Rep, class Period>
    timer(
        capy::execution_context& ctx,
        std::chrono::duration<Rep, Period> d)
        : timer(ctx)
    {
        expires_after(d);
    }

    /** Move constructor.

        Transfers ownership of the timer resources.

        @param other The timer to move from.
    */
    timer(timer&& other) noexcept;

    /** Move assignment operator.

        Closes any existing timer and transfers ownership.
        The source and destination must share the same execution context.

        @param other The timer to move from.

        @return Reference to this timer.

        @throws std::logic_error if the timers have different execution contexts.
    */
    timer& operator=(timer&& other);

    timer(timer const&) = delete;
    timer& operator=(timer const&) = delete;

    /** Cancel all pending asynchronous wait operations.

        All outstanding operations complete with an error code that
        compares equal to `capy::cond::canceled`.

        @return The number of operations that were cancelled.
    */
    std::size_t cancel()
    {
        if (!get().might_have_pending_waits_)
            return 0;
        return do_cancel();
    }

    /** Cancel one pending asynchronous wait operation.

        The oldest pending wait is cancelled (FIFO order). It
        completes with an error code that compares equal to
        `capy::cond::canceled`.

        @return The number of operations that were cancelled (0 or 1).
    */
    std::size_t cancel_one()
    {
        if (!get().might_have_pending_waits_)
            return 0;
        return do_cancel_one();
    }

    /** Return the timer's expiry time as an absolute time.

        @return The expiry time point. If no expiry has been set,
            returns a default-constructed time_point.
    */
    time_point expiry() const noexcept
    {
        return get().expiry_;
    }

    /** Set the timer's expiry time as an absolute time.

        Any pending asynchronous wait operations will be cancelled.

        @param t The expiry time to be used for the timer.

        @return The number of pending operations that were cancelled.
    */
    std::size_t expires_at(time_point t)
    {
        auto& impl = get();
        impl.expiry_ = t;
        if (impl.heap_index_ == timer_impl::npos &&
            !impl.might_have_pending_waits_)
            return 0;
        return do_update_expiry();
    }

    /** Set the timer's expiry time relative to now.

        Any pending asynchronous wait operations will be cancelled.

        @param d The expiry time relative to now.

        @return The number of pending operations that were cancelled.
    */
    std::size_t expires_after(duration d)
    {
        auto& impl = get();
        if (d <= duration::zero())
            impl.expiry_ = (time_point::min)();
        else
            impl.expiry_ = clock_type::now() + d;
        if (impl.heap_index_ == timer_impl::npos &&
            !impl.might_have_pending_waits_)
            return 0;
        return do_update_expiry();
    }

    /** Set the timer's expiry time relative to now.

        This is a convenience overload that accepts any duration type
        and converts it to the timer's native duration type. Any
        pending asynchronous wait operations will be cancelled.

        @param d The expiry time relative to now.

        @return The number of pending operations that were cancelled.
    */
    template<class Rep, class Period>
    std::size_t expires_after(std::chrono::duration<Rep, Period> d)
    {
        return expires_after(std::chrono::duration_cast<duration>(d));
    }

    /** Wait for the timer to expire.

        Multiple coroutines may wait on the same timer concurrently.
        When the timer expires, all waiters complete with success.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, only that waiter completes with an error that
        compares equal to `capy::cond::canceled`; other waiters are
        unaffected.

        @par Example
        @code
        timer t(ctx);
        t.expires_after(std::chrono::seconds(5));
        auto [ec] = co_await t.wait();
        if (ec == capy::cond::canceled)
        {
            // Cancelled via stop_token or cancel()
            co_return;
        }
        if (ec)
        {
            // Handle other errors
            co_return;
        }
        // Timer expired
        @endcode

        @return An awaitable that completes with `io_result<>`.
            Returns success (default error_code) when the timer expires,
            or an error code on failure. Compare against error conditions
            (e.g., `ec == capy::cond::canceled`) rather than error codes.

        @par Preconditions
        The timer must have an expiry time set via expires_at() or
        expires_after().
    */
    auto wait()
    {
        return wait_awaitable(*this);
    }

private:
    // Out-of-line cancel/expiry when inline fast-path
    // conditions (no waiters, not in heap) are not met.
    std::size_t do_cancel();
    std::size_t do_cancel_one();
    std::size_t do_update_expiry();

    /// Return the underlying implementation.
    timer_impl& get() const noexcept
    {
        return *static_cast<timer_impl*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
