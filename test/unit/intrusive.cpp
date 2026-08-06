//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/intrusive.hpp>

#include <vector>

#include "test_suite.hpp"

using namespace boost::corosio::detail;

namespace {

struct list_item : intrusive_list<list_item>::node
{
    int id;
    explicit list_item(int i) : id(i) {}
};

struct queue_item : intrusive_queue<queue_item>::node
{
    int id;
    explicit queue_item(int i) : id(i) {}
};

} // namespace

struct intrusive_test
{
    void testListPushPop()
    {
        intrusive_list<list_item> l;
        BOOST_TEST(l.empty());
        BOOST_TEST(l.front() == nullptr);
        BOOST_TEST(l.pop_front() == nullptr);

        list_item a{1}, b{2}, c{3};
        l.push_back(&a);
        l.push_back(&b);
        l.push_back(&c);
        BOOST_TEST(!l.empty());
        BOOST_TEST(l.front() == &a);

        BOOST_TEST(l.pop_front() == &a);
        BOOST_TEST(l.pop_front() == &b);
        BOOST_TEST(l.pop_front() == &c);
        BOOST_TEST(l.empty());
    }

    void testListRemove()
    {
        // Remove from the middle, head, and tail.
        intrusive_list<list_item> l;
        list_item a{1}, b{2}, c{3};
        l.push_back(&a);
        l.push_back(&b);
        l.push_back(&c);

        l.remove(&b);
        BOOST_TEST(l.front() == &a);
        l.remove(&a);
        BOOST_TEST(l.front() == &c);
        l.remove(&c);
        BOOST_TEST(l.empty());

        // Removing a node that was already popped is a no-op.
        l.push_back(&a);
        l.push_back(&b);
        BOOST_TEST(l.pop_front() == &a);
        l.remove(&a);
        BOOST_TEST(l.front() == &b);
        BOOST_TEST(l.pop_front() == &b);
    }

    void testListForEach()
    {
        intrusive_list<list_item> l;
        list_item a{1}, b{2}, c{3};
        l.push_back(&a);
        l.push_back(&b);
        l.push_back(&c);

        std::vector<int> seen;
        l.for_each([&](list_item* p) { seen.push_back(p->id); });
        BOOST_TEST_EQ(seen.size(), 3u);
        BOOST_TEST_EQ(seen[0], 1);
        BOOST_TEST_EQ(seen[1], 2);
        BOOST_TEST_EQ(seen[2], 3);
    }

    void testListSpliceBack()
    {
        list_item a{1}, b{2}, c{3}, d{4};

        // Splice from an empty list is a no-op.
        {
            intrusive_list<list_item> dst, src;
            dst.push_back(&a);
            dst.splice_back(src);
            BOOST_TEST(dst.front() == &a);
            BOOST_TEST(dst.pop_front() == &a);
            BOOST_TEST(dst.empty());
        }

        // Splice into an empty list adopts the source whole.
        {
            intrusive_list<list_item> dst, src;
            src.push_back(&a);
            src.push_back(&b);
            dst.splice_back(src);
            BOOST_TEST(src.empty());
            BOOST_TEST(dst.pop_front() == &a);
            BOOST_TEST(dst.pop_front() == &b);
        }

        // Splice appends behind existing elements.
        {
            intrusive_list<list_item> dst, src;
            dst.push_back(&a);
            dst.push_back(&b);
            src.push_back(&c);
            src.push_back(&d);
            dst.splice_back(src);
            BOOST_TEST(src.empty());
            BOOST_TEST(dst.pop_front() == &a);
            BOOST_TEST(dst.pop_front() == &b);
            BOOST_TEST(dst.pop_front() == &c);
            BOOST_TEST(dst.pop_front() == &d);
            BOOST_TEST(dst.empty());
        }
    }

    void testListMove()
    {
        intrusive_list<list_item> src;
        list_item a{1};
        src.push_back(&a);

        intrusive_list<list_item> dst(std::move(src));
        BOOST_TEST(src.empty());
        BOOST_TEST(dst.pop_front() == &a);
    }

    void testQueuePushPop()
    {
        intrusive_queue<queue_item> q;
        BOOST_TEST(q.empty());
        BOOST_TEST(q.pop() == nullptr);

        queue_item a{1}, b{2};
        q.push(&a);
        q.push(&b);
        BOOST_TEST(!q.empty());
        BOOST_TEST(q.pop() == &a);
        BOOST_TEST(q.pop() == &b);
        BOOST_TEST(q.empty());
    }

    void testQueueSplice()
    {
        queue_item a{1}, b{2}, c{3};

        // Splice from an empty queue is a no-op.
        {
            intrusive_queue<queue_item> dst, src;
            dst.push(&a);
            dst.splice(src);
            BOOST_TEST(dst.pop() == &a);
            BOOST_TEST(dst.empty());
        }

        // Splice into an empty queue adopts the source whole.
        {
            intrusive_queue<queue_item> dst, src;
            src.push(&a);
            src.push(&b);
            dst.splice(src);
            BOOST_TEST(src.empty());
            BOOST_TEST(dst.pop() == &a);
            BOOST_TEST(dst.pop() == &b);
        }

        // Splice appends behind existing elements.
        {
            intrusive_queue<queue_item> dst, src;
            dst.push(&a);
            src.push(&b);
            src.push(&c);
            dst.splice(src);
            BOOST_TEST(src.empty());
            BOOST_TEST(dst.pop() == &a);
            BOOST_TEST(dst.pop() == &b);
            BOOST_TEST(dst.pop() == &c);
        }
    }

    void testQueueMove()
    {
        intrusive_queue<queue_item> src;
        queue_item a{1};
        src.push(&a);

        intrusive_queue<queue_item> dst(std::move(src));
        BOOST_TEST(src.empty());
        BOOST_TEST(dst.pop() == &a);
    }

    void run()
    {
        testListPushPop();
        testListRemove();
        testListForEach();
        testListSpliceBack();
        testListMove();
        testQueuePushPop();
        testQueueSplice();
        testQueueMove();
    }
};

TEST_SUITE(intrusive_test, "boost.corosio.intrusive");
