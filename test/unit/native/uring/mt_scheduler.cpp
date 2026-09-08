//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// io_uring scheduler paths that need contended, multi-threaded dispatch: a
// follower parked in cond_.wait_for while another thread holds the ring
// leadership, woken by work posted from a foreign thread. Durations only
// bound blocking; nothing asserts elapsed time.

#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace boost::corosio {

struct uring_mt_scheduler_test
{
    void testForeignPostWakesParkedFollower()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // A parked read keeps outstanding work alive so both runner threads
        // stay inside the scheduler: one leads the ring, the other parks in
        // cond_.wait_for. Each foreign post then has a follower to wake.
        // The closure is a named local (not a temporary), so the suspended
        // coroutine's captures outlive it.
        char buf[4];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = ec;
            std::ignore = n;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::atomic<int> entered{0};
        std::atomic<int> nops{0};
        // A fixed slice count (rather than a work-gated one) keeps both
        // threads in the scheduler through quiet windows: with the read
        // parked, one thread leads the ring while the other parks in
        // cond_.wait_for and times out, then they trade roles.
        auto slice = [&] {
            entered.fetch_add(1);
            for (int i = 0; i < 8; ++i)
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(5));
        };
        std::thread ra(slice), rb(slice);

        while (entered.load() < 2)
        {
        }
        for (int i = 0; i < 20; ++i)
        {
            // The counter travels as a parameter: a loop-scoped closure
            // would die before the runner threads execute the frame that
            // references it.
            capy::run_async(ex)([](std::atomic<int>* n) -> capy::task<> {
                n->fetch_add(1);
                co_return;
            }(&nops));
        }
        ra.join();
        rb.join();

        // Drain any posts the fixed slices did not reach, plus the read.
        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST_EQ(nops.load(), 20);
        BOOST_TEST(resumed);
    }

    void run()
    {
        testForeignPostWakesParkedFollower();
    }
};

TEST_SUITE(uring_mt_scheduler_test, "boost.corosio.native.uring.mt_scheduler");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
