//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/ready_queue.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/continuation.hpp>

#include "test_suite.hpp"

using namespace boost::corosio::detail;

namespace {

struct fake_op : scheduler_op
{
    int id;
    explicit fake_op(int i) : id(i) {}
    void operator()() override {}
};

} // namespace

struct ready_queue_test
{
    void testFifoOpsOnly()
    {
        ready_queue q;
        BOOST_TEST(q.empty());
        fake_op a{1}, b{2};
        q.push(&a);
        q.push(&b);
        BOOST_TEST(!q.empty());

        auto e1 = q.pop();
        BOOST_TEST(!ready_is_continuation(e1));
        BOOST_TEST(ready_as_op(e1) == &a);

        auto e2 = q.pop();
        BOOST_TEST(ready_as_op(e2) == &b);

        BOOST_TEST(q.pop() == 0u);
        BOOST_TEST(q.empty());
    }

    void testMixedOrderPreserved()
    {
        ready_queue q;
        fake_op a{1};
        boost::capy::continuation c{};
        fake_op b{2};
        q.push(&a);
        q.push(c);
        q.push(&b);

        auto e1 = q.pop();
        BOOST_TEST(!ready_is_continuation(e1));
        BOOST_TEST(ready_as_op(e1) == &a);

        auto e2 = q.pop();
        BOOST_TEST(ready_is_continuation(e2));
        BOOST_TEST(ready_as_cont(e2) == &c);

        auto e3 = q.pop();
        BOOST_TEST(ready_as_op(e3) == &b);
    }

    void testSpliceAppendsInOrder()
    {
        ready_queue q1, q2;
        fake_op a{1}, b{2};
        q1.push(&a);
        q2.push(&b);
        q1.splice(q2);
        BOOST_TEST(q2.empty());
        BOOST_TEST(ready_as_op(q1.pop()) == &a);
        BOOST_TEST(ready_as_op(q1.pop()) == &b);
        BOOST_TEST(q1.pop() == 0u);
    }

    void run()
    {
        testFifoOpsOnly();
        testMixedOrderPreserved();
        testSpliceAppendsInOrder();
    }
};

TEST_SUITE(ready_queue_test, "boost.corosio.ready_queue");
