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
#include <boost/corosio/io/io_timer.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/concept/executor.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <type_traits>

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
    Timers are not backed by per-timer kernel objects. The io_context's
    timer service keeps a process-side min-heap of pending expirations;
    the nearest expiry drives the reactor's poll timeout, and expirations
    are processed in the run loop.
*/
class BOOST_COROSIO_DECL timer : public io_timer
{
public:
    /// Alias for backward compatibility.
    using implementation = io_timer::implementation;

    /** Destructor.

        Cancels any pending operations and releases timer resources.
    */
    ~timer() override;

    /** Construct a timer from an execution context.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).

        @throws std::logic_error if @p ctx is not an io_context.
    */
    explicit timer(capy::execution_context& ctx);

    /** Construct a timer with an initial absolute expiry time.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).
        @param t The initial expiry time point.

        @throws std::logic_error if @p ctx is not an io_context.
    */
    timer(capy::execution_context& ctx, time_point t);

    /** Construct a timer with an initial relative expiry time.

        @param ctx The execution context that will own this timer. It
            must be a corosio io_context; otherwise the constructor
            throws (a timer service is required).
        @param d The initial expiry duration relative to now.

        @throws std::logic_error if @p ctx is not an io_context.
    */
    template<class Rep, class Period>
    timer(capy::execution_context& ctx, std::chrono::duration<Rep, Period> d)
        : timer(ctx)
    {
        expires_after(d);
    }

    /** Construct a timer from an executor.

        The timer is associated with the executor's context, which must
        be a corosio io_context.

        @param ex The executor whose context will own this timer.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, timer>) &&
        capy::Executor<Ex>
    explicit timer(Ex const& ex) : timer(ex.context())
    {
    }

    /** Construct a timer from an executor with an absolute expiry time.

        @param ex The executor whose context will own this timer.
        @param t The initial expiry time point.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex>
        requires capy::Executor<Ex>
    timer(Ex const& ex, time_point t) : timer(ex.context(), t)
    {
    }

    /** Construct a timer from an executor with a relative expiry time.

        @param ex The executor whose context will own this timer.
        @param d The initial expiry duration relative to now.

        @throws std::logic_error if the executor's context is not an
            io_context.
    */
    template<class Ex, class Rep, class Period>
        requires capy::Executor<Ex>
    timer(Ex const& ex, std::chrono::duration<Rep, Period> d)
        : timer(ex.context(), d)
    {
    }

    /** Move constructor.

        Transfers ownership of the timer resources.

        @param other The timer to move from.

        @pre No awaitables returned by @p other's methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.
    */
    timer(timer&& other) noexcept;

    /** Move assignment operator.

        Closes any existing timer and transfers ownership.

        @param other The timer to move from.

        @pre No awaitables returned by either `*this` or @p other's
            methods exist.
        @pre The execution context associated with @p other must
            outlive this timer.

        @return Reference to this timer.
    */
    timer& operator=(timer&& other) noexcept;

    timer(timer const&)            = delete;
    timer& operator=(timer const&) = delete;

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

    /** Set the timer's expiry time as an absolute time.

        Any pending asynchronous wait operations will be cancelled.

        @param t The expiry time to be used for the timer.

        @return The number of pending operations that were cancelled.
    */
    std::size_t expires_at(time_point t)
    {
        auto& impl   = get();
        impl.expiry_ = t;
        if (impl.heap_index_ == implementation::npos &&
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
        if (impl.heap_index_ == implementation::npos &&
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

protected:
    explicit timer(handle h) noexcept : io_timer(std::move(h)) {}

private:
    std::size_t do_cancel() override;
    std::size_t do_cancel_one();
    std::size_t do_update_expiry();

    /// Return the underlying implementation.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
