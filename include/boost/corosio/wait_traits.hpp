//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WAIT_TRAITS_HPP
#define BOOST_COROSIO_WAIT_TRAITS_HPP

#include <boost/corosio/detail/config.hpp>

#include <concepts>

namespace boost::corosio {

/** Default wait traits for clock-based delays.

    Controls how much of the remaining time a single underlying
    steady-clock wait may cover before `Clock::now()` is re-read.
    A larger value costs fewer wakeups; a smaller value bounds how
    late an adjustment of `Clock` ( e.g. a stepped time-of-day
    clock ) is observed. The default covers the full remaining
    duration, which is exact for clocks that advance in lockstep
    with the machine's monotonic clock.

    @par Example
    @code
    // Observe wall-clock steps within one second
    struct capped_traits
    {
        static std::chrono::system_clock::duration
        to_wait_duration(std::chrono::system_clock::duration d)
        {
            return (std::min)(d,
                std::chrono::system_clock::duration(
                    std::chrono::seconds(1)));
        }
    };

    auto [ec] = co_await delay<capped_traits>(
        std::chrono::system_clock::now() + std::chrono::hours(1));
    @endcode

    @tparam Clock The clock type whose durations are converted.

    @see delay
*/
template<class Clock>
struct wait_traits
{
    /** Convert a remaining duration into a wait duration.

        Should return a positive duration when @p d is positive; a
        non-positive result degrades to reactor-rate re-checking.

        @par Preconditions
        Must not throw and must not block — invoked on the
        io_context's run thread, including from the timer
        completion path.

        @param d The remaining time until the deadline.

        @return The duration the next underlying wait may cover.
    */
    static typename Clock::duration
    to_wait_duration(typename Clock::duration d)
    {
        return d;
    }
};

/** Concept for wait-traits policies usable with `Clock`.

    Satisfied when `Traits::to_wait_duration` accepts a
    `Clock::duration` and returns something convertible back to it.
    `Traits::to_wait_duration` must not throw.
*/
template<class Traits, class Clock>
concept WaitTraits = requires(typename Clock::duration d)
{
    { Traits::to_wait_duration(d) }
        -> std::convertible_to<typename Clock::duration>;
};

} // namespace boost::corosio

#endif
