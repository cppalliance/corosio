//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TIMEOUT_HPP
#define BOOST_COROSIO_TIMEOUT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/timeout_awaitable.hpp>
#include <boost/capy/concept/io_awaitable.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

namespace boost::corosio {

/** Race an io_result-returning awaitable against a deadline.

    Starts the awaitable with an interposed stop token and arms a
    timer. If the awaitable finishes first, its result is returned
    as-is (success, error, or exception). If the deadline passes
    first, the awaitable is cancelled and an `io_result` whose
    `ec` compares equal to `capy::cond::timeout` (with a
    default-initialized payload) is produced.

    Exceptions from the inner awaitable always propagate; they are
    never swallowed by the timer.

    @par Preconditions
    The awaiting coroutine's executor must belong to an
    `io_context`; any other execution context terminates with a
    diagnostic.

    @par Cancellation
    If the parent's stop token is activated, the inner awaitable
    is cancelled and its cancellation result is returned. Requesting
    stop from another thread requires a multi-threaded-capable
    io_context; a context running in single_threaded mode
    (auto-enabled at concurrency_hint == 1) does not permit
    cross-thread cancellation.

    @par Example
    @code
    auto [ec, n] = co_await timeout(sock.read_some(buf), 50ms);
    if (ec == capy::cond::timeout) {
        // handle timeout
    }
    @endcode

    @param a The awaitable to race against the deadline.
    @param dur The maximum duration to wait, measured from
        suspension.

    @return An awaitable yielding `io_result` matching the inner
        awaitable's result type.

    @see delay
*/
template<capy::IoAwaitable A, typename Rep, typename Period>
    requires detail::is_io_result_v<
            std::remove_cvref_t<capy::awaitable_result_t<A>>> &&
        std::is_default_constructible_v<
            std::remove_cvref_t<capy::awaitable_result_t<A>>>
[[nodiscard]] auto timeout(A a, std::chrono::duration<Rep, Period> dur)
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
    return detail::timeout_awaitable<A>(std::move(a), ns);
}

/** Race an io_result-returning awaitable against an absolute deadline.

    Behaves as the duration overload with the deadline fixed at
    `tp` instead of measured from suspension.

    @param a The awaitable to race against the deadline.
    @param tp The steady-clock time point at which the awaitable
        is cancelled.

    @return An awaitable yielding `io_result` matching the inner
        awaitable's result type.

    @see delay
*/
template<capy::IoAwaitable A>
    requires detail::is_io_result_v<
            std::remove_cvref_t<capy::awaitable_result_t<A>>> &&
        std::is_default_constructible_v<
            std::remove_cvref_t<capy::awaitable_result_t<A>>>
[[nodiscard]] auto timeout(A a, std::chrono::steady_clock::time_point tp)
{
    return detail::timeout_awaitable<A>(std::move(a), tp);
}

} // namespace boost::corosio

#endif
