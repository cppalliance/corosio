//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/detail/reactor/reactor_defer_policy.hpp>

#include "test_suite.hpp"

using boost::corosio::detail::reactor_defer_policy;

namespace {

// Drive the policy as the scheduler does for `turns` sentinel turns that
// find handlers queued; `queued_on_deferred_turn(i)` says whether the i-th
// deferral's turn still found handlers. Returns the deferrals made.
template<class F>
int
drive(reactor_defer_policy& p, int turns, F queued_on_deferred_turn)
{
    int deferrals = 0;
    for (int t = 0; t < turns; ++t)
    {
        if (p.may_defer())
            p.on_deferred_turn(queued_on_deferred_turn(deferrals++));
        else
            p.on_polled_instead();
    }
    return deferrals;
}

} // namespace

struct reactor_defer_policy_test
{
    void testFreshPolicyDefers()
    {
        reactor_defer_policy p;
        BOOST_TEST(p.may_defer());
    }

    void testDeferralsThatPayKeepDeferring()
    {
        reactor_defer_policy p;
        int const n = drive(p, 1000, [](int) { return true; });
        BOOST_TEST_EQ(n, 1000);
        BOOST_TEST(p.may_defer());
    }

    void testEmptiedQueueStopsDeferring()
    {
        reactor_defer_policy p;
        p.on_deferred_turn(false);
        BOOST_TEST(!p.may_defer());
    }

    void testPollingRetriesDeferral()
    {
        reactor_defer_policy p;
        p.on_deferred_turn(false);
        for (int i = 0; i < reactor_defer_policy::miss_cost -
                 reactor_defer_policy::threshold + 1;
             ++i)
            p.on_polled_instead();
        BOOST_TEST(p.may_defer());
    }

    // Losing every time still tries again, but only on about one turn in
    // `miss_cost + 1`.
    void testConstantLossesBackOff()
    {
        reactor_defer_policy p;
        int const turns = 9000;
        int const n     = drive(p, turns, [](int) { return false; });
        BOOST_TEST(n > 0);
        BOOST_TEST(n <= turns / (reactor_defer_policy::miss_cost + 1) + 2);
    }

    // A loss in fewer than one deferral in `miss_cost + 1` costs only the
    // turn after it; every other turn still defers.
    void testRareLossesKeepDeferring()
    {
        reactor_defer_policy p;
        int const period = reactor_defer_policy::miss_cost + 2;
        int const turns  = 1000;
        int const n = drive(p, turns, [&](int i) { return i % period != 0; });
        int const losses = (n + period - 1) / period;
        BOOST_TEST_EQ(turns - n, losses);
    }

    // However long a workload loses, a later change is tried within a
    // bounded number of polls.
    void testLossesAreBounded()
    {
        reactor_defer_policy p;
        for (int i = 0; i < 1000; ++i)
            p.on_deferred_turn(false);
        for (int i = 0; i < reactor_defer_policy::max_score -
                 reactor_defer_policy::threshold + 1;
             ++i)
            p.on_polled_instead();
        BOOST_TEST(p.may_defer());
    }

    void testResetForgetsLosses()
    {
        reactor_defer_policy p;
        p.on_deferred_turn(false);
        p.reset();
        BOOST_TEST(p.may_defer());
    }

    void run()
    {
        testFreshPolicyDefers();
        testDeferralsThatPayKeepDeferring();
        testEmptiedQueueStopsDeferring();
        testPollingRetriesDeferral();
        testConstantLossesBackOff();
        testRareLossesKeepDeferring();
        testLossesAreBounded();
        testResetForgetsLosses();
    }
};

TEST_SUITE(reactor_defer_policy_test, "boost.corosio.reactor_defer_policy");
