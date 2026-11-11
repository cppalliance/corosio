//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DEFER_POLICY_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DEFER_POLICY_HPP

namespace boost::corosio::detail {

/** Decide whether the reactor sentinel waits behind queued handlers.

    When the sentinel reaches the front of the queue with handlers still
    queued, running those handlers first usually saves a reactor pass:
    they tend to start the next I/O themselves. That bet loses when the
    handlers drain the queue, so the deferred turn has no other work to
    overlap with and a poll that would have found completed I/O comes too
    late. The policy scores each deferral by what the deferred turn found
    and stops deferring while losses are frequent.

    A deferred turn that found the queue empty costs more than one that
    found handlers repays, so deferring continues only while fewer than
    about one deferral in `miss_cost + 1` empties the queue. Each turn
    that polls instead of deferring lets the score decay, so a workload
    that changes is tried again.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. The scheduler calls it under its own mutex.
*/
class reactor_defer_policy
{
    int score_ = 0;

public:
    /// Score added by a deferred turn that found the queue empty.
    static constexpr int miss_cost = 8;

    /// Score at or above which the sentinel polls instead of deferring.
    static constexpr int threshold = 8;

    /// Highest score, which bounds how long deferring stays off.
    static constexpr int max_score = 64;

    /// Check whether the sentinel may wait behind queued handlers.
    bool may_defer() const noexcept
    {
        return score_ < threshold;
    }

    /** Record what a deferred turn of the sentinel found.

        @param handlers_queued `true` if handlers were still queued, so
            the deferral cost nothing.
    */
    void on_deferred_turn(bool handlers_queued) noexcept
    {
        if (handlers_queued)
        {
            if (score_ > 0)
                --score_;
        }
        else
        {
            score_ =
                score_ > max_score - miss_cost ? max_score : score_ + miss_cost;
        }
    }

    /// Record a turn that polled because deferring was off.
    void on_polled_instead() noexcept
    {
        if (score_ > 0)
            --score_;
    }

    /// Forget the history, as for a fresh run.
    void reset() noexcept
    {
        score_ = 0;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DEFER_POLICY_HPP
