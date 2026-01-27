//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
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
#include <boost/capy/concept/executor.hpp>

#include <chrono>
#include <concepts>
#include <coroutine>
#include <stop_token>
#include <type_traits>

namespace boost::corosio {

/** An asynchronous timer for coroutine I/O.

    This class provides asynchronous timer operations that return
    awaitable types. The timer can be used to schedule operations
    to occur after a specified duration or at a specific time point.

    Each timer operation participates in the affine awaitable protocol,
    ensuring coroutines resume on the correct executor.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe. A timer must not have concurrent wait
    operations.
*/
class BOOST_COROSIO_DECL timer : public io_object
{
    struct wait_awaitable
    {
        timer& t_;
        std::stop_token token_;
        mutable system::error_code ec_;

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

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex) -> std::coroutine_handle<>
        {
            t_.get().wait(h, ex, token_, &ec_);
            return std::noop_coroutine();
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            t_.get().wait(h, ex, token_, &ec_);
            return std::noop_coroutine();
        }
    };

public:
    struct timer_impl : io_object_impl
    {
        virtual void wait(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            system::error_code*) = 0;
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

    /** Cancel any pending asynchronous operations.

        All outstanding operations complete with @ref capy::error::canceled.
        Check `ec == capy::cond::canceled` for portable comparison.
    */
    void cancel();

    /** Get the timer's expiry time as an absolute time.

        @return The expiry time point. If no expiry has been set,
            returns a default-constructed time_point.
    */
    time_point expiry() const;

    /** Set the timer's expiry time as an absolute time.

        Any pending asynchronous wait operations will be cancelled.

        @param t The expiry time to be used for the timer.
    */
    void expires_at(time_point t);

    /** Set the timer's expiry time relative to now.

        Any pending asynchronous wait operations will be cancelled.

        @param d The expiry time relative to now.
    */
    void expires_after(duration d);

    /** Set the timer's expiry time relative to now.

        This is a convenience overload that accepts any duration type
        and converts it to the timer's native duration type.

        @param d The expiry time relative to now.
    */
    template<class Rep, class Period>
    void expires_after(std::chrono::duration<Rep, Period> d)
    {
        expires_after(std::chrono::duration_cast<duration>(d));
    }

    /** Wait for the timer to expire.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `capy::error::canceled`.

        @return An awaitable that completes with `io_result<>`.
            Returns success (default error_code) when the timer expires,
            or an error code on failure including:
            - capy::error::canceled: Cancelled via stop_token or cancel().
                Check `ec == cond::canceled` for portable comparison.

        @par Preconditions
        The timer must have an expiry time set via expires_at() or
        expires_after().
    */
    auto wait()
    {
        return wait_awaitable(*this);
    }

private:
    timer_impl& get() const noexcept
    {
        return *static_cast<timer_impl*>(impl_);
    }
};

} // namespace boost::corosio

#endif
