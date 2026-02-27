//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_CANCEL_HPP
#define BOOST_COROSIO_CANCEL_HPP

#include <boost/corosio/detail/cancel_at_awaitable.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/concept/io_awaitable.hpp>

#include <type_traits>
#include <utility>

namespace boost::corosio {

/** Cancel an operation if it does not complete by a deadline.

    Races @p op against the given timer. If the deadline is reached
    first, the inner operation is cancelled via its stop token and
    completes with an error comparing equal to `capy::cond::canceled`.
    If the inner operation completes first, the timer is cancelled.

    Parent cancellation (from the caller's stop token) is forwarded
    to both the inner operation and the timeout timer.

    The timer's expiry is overwritten by this call. The timer must
    outlive the returned awaitable. Do not issue overlapping waits
    on the same timer.

    @par Completion Conditions
    The returned awaitable resumes when either:
    @li The inner operation completes (successfully or with error).
    @li The deadline expires and the inner operation is cancelled.
    @li The caller's stop token is triggered, cancelling both.

    @par Error Conditions
    @li On timeout or parent cancellation, the inner operation
        completes with an error equal to `capy::cond::canceled`.
    @li All other errors are propagated from the inner operation.

    @par Example
    @code
    timer timeout_timer( ioc );
    auto [ec, n] = co_await cancel_at(
        sock.read_some( buf ), timeout_timer,
        clock::now() + 5s );
    if (ec == capy::cond::canceled)
        // timed out or parent cancelled
    @endcode

    @param op The inner I/O awaitable to wrap.
    @param t The timer to use for the deadline. Must outlive
        the returned awaitable.
    @param deadline The absolute time point at which to cancel.

    @return An awaitable whose result matches @p op's result type.

    @see cancel_after
*/
auto
cancel_at(capy::IoAwaitable auto&& op, timer& t, timer::time_point deadline)
{
    return detail::cancel_at_awaitable<std::decay_t<decltype(op)>, timer>(
        std::forward<decltype(op)>(op), t, deadline);
}

/** Cancel an operation if it does not complete within a duration.

    Equivalent to `cancel_at( op, t, clock::now() + timeout )`.

    The timer's expiry is overwritten by this call. The timer must
    outlive the returned awaitable. Do not issue overlapping waits
    on the same timer.

    @par Completion Conditions
    The returned awaitable resumes when either:
    @li The inner operation completes (successfully or with error).
    @li The timeout elapses and the inner operation is cancelled.
    @li The caller's stop token is triggered, cancelling both.

    @par Error Conditions
    @li On timeout or parent cancellation, the inner operation
        completes with an error equal to `capy::cond::canceled`.
    @li All other errors are propagated from the inner operation.

    @par Example
    @code
    timer timeout_timer( ioc );
    auto [ec, n] = co_await cancel_after(
        sock.read_some( buf ), timeout_timer, 5s );
    if (ec == capy::cond::canceled)
        // timed out
    @endcode

    @param op The inner I/O awaitable to wrap.
    @param t The timer to use for the timeout. Must outlive
        the returned awaitable.
    @param timeout The relative duration after which to cancel.

    @return An awaitable whose result matches @p op's result type.

    @see cancel_at
*/
auto
cancel_after(capy::IoAwaitable auto&& op, timer& t, timer::duration timeout)
{
    return cancel_at(
        std::forward<decltype(op)>(op), t, timer::clock_type::now() + timeout);
}

/** Cancel an operation if it does not complete by a deadline.

    Convenience overload that creates a @ref timer internally.
    Otherwise identical to the explicit-timer overload.

    @par Completion Conditions
    The returned awaitable resumes when either:
    @li The inner operation completes (successfully or with error).
    @li The deadline expires and the inner operation is cancelled.
    @li The caller's stop token is triggered, cancelling both.

    @par Error Conditions
    @li On timeout or parent cancellation, the inner operation
        completes with an error equal to `capy::cond::canceled`.
    @li All other errors are propagated from the inner operation.

    @note Creates a timer per call. Use the explicit-timer overload
        to amortize allocation across multiple timeouts.

    @par Example
    @code
    auto [ec, n] = co_await cancel_at(
        sock.read_some( buf ),
        clock::now() + 5s );
    if (ec == capy::cond::canceled)
        // timed out or parent cancelled
    @endcode

    @param op The inner I/O awaitable to wrap.
    @param deadline The absolute time point at which to cancel.

    @return An awaitable whose result matches @p op's result type.

    @see cancel_after
*/
auto
cancel_at(capy::IoAwaitable auto&& op, timer::time_point deadline)
{
    return detail::cancel_at_awaitable<std::decay_t<decltype(op)>, timer, true>(
        std::forward<decltype(op)>(op), deadline);
}

/** Cancel an operation if it does not complete within a duration.

    Convenience overload that creates a @ref timer internally.
    Equivalent to `cancel_at( op, clock::now() + timeout )`.

    @par Completion Conditions
    The returned awaitable resumes when either:
    @li The inner operation completes (successfully or with error).
    @li The timeout elapses and the inner operation is cancelled.
    @li The caller's stop token is triggered, cancelling both.

    @par Error Conditions
    @li On timeout or parent cancellation, the inner operation
        completes with an error equal to `capy::cond::canceled`.
    @li All other errors are propagated from the inner operation.

    @note Creates a timer per call. Use the explicit-timer overload
        to amortize allocation across multiple timeouts.

    @par Example
    @code
    auto [ec, n] = co_await cancel_after(
        sock.read_some( buf ), 5s );
    if (ec == capy::cond::canceled)
        // timed out
    @endcode

    @param op The inner I/O awaitable to wrap.
    @param timeout The relative duration after which to cancel.

    @return An awaitable whose result matches @p op's result type.

    @see cancel_at
*/
auto
cancel_after(capy::IoAwaitable auto&& op, timer::duration timeout)
{
    return cancel_at(
        std::forward<decltype(op)>(op), timer::clock_type::now() + timeout);
}

} // namespace boost::corosio

#endif
