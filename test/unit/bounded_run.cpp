//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Bounded run variants (run_for / run_one_for / poll) on the reactor
// schedulers. Durations here bound how long a call may block; nothing
// asserts elapsed time, only that parked work stays parked and armed
// timers still fire within the bound.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || \
    BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <thread>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct bounded_run_test
{
    void testRunForLeavesParkedReadParked()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        char buf[8];
        std::error_code rec;
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::ignore = ioc.run_for(std::chrono::milliseconds(50));
        BOOST_TEST(!resumed);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    void testRunOneForLeavesParkedReadParked()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        char buf[8];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            std::ignore = ec;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        // First slice starts the reader coroutine and parks it; the
        // second finds only the parked read and waits out the bound.
        std::ignore = ioc.run_one_for(std::chrono::milliseconds(20));
        std::ignore = ioc.run_one_for(std::chrono::milliseconds(20));
        BOOST_TEST(!resumed);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
    }

    void testRunForFiresArmedDelay()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        bool fired = false;
        auto task  = [&]() -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            fired = true;
        };
        capy::run_async(ex)(task());

        // The bound exceeds the delay, so the scheduler's timed wait
        // must be capped by the timer and the delay must complete.
        std::ignore = ioc.run_for(std::chrono::seconds(20));
        BOOST_TEST(fired);
    }

    void testPollLeavesParkedReadParked()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        char buf[8];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            std::ignore = ec;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::ignore = ioc.poll();
        BOOST_TEST(!resumed);
        ioc.restart();
        std::ignore = ioc.poll_one();
        BOOST_TEST(!resumed);

        // Queued work alongside the parked read makes the reactor pass
        // run with a zero timeout instead of blocking.
        bool posted = false;
        auto nop    = [&]() -> capy::task<> {
            posted = true;
            co_return;
        };
        capy::run_async(ex)(nop());
        ioc.restart();
        std::ignore = ioc.poll();
        BOOST_TEST(posted);
        BOOST_TEST(!resumed);

        // With nothing queued the reactor pass itself runs with a
        // zero timeout.
        ioc.restart();
        std::ignore = ioc.poll();
        BOOST_TEST(!resumed);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
    }

    void testTwoThreadBoundedFollower()
    {
        // Two threads inside the scheduler: one holds reactor
        // leadership on a parked read, the other's bounded slices take
        // the follower timed wait. Bounds cap blocking; nothing
        // asserts elapsed time.
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        char buf[8];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            std::ignore = ec;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::thread follower([&] {
            for (int i = 0; i < 5 && !resumed; ++i)
            {
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(20));
                std::ignore = ioc.poll();
            }
        });
        for (int i = 0; i < 5 && !resumed; ++i)
            std::ignore = ioc.run_one_for(std::chrono::milliseconds(20));
        follower.join();
        BOOST_TEST(!resumed);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
    }


    void testOversizeBudgetThrows()
    {
        io_context_options opts;
        opts.inline_budget_max =
            (std::numeric_limits<unsigned>::max)();
        BOOST_TEST_THROWS(
            ([&] { io_context tmp(Backend, opts); }()), std::out_of_range);
    }

    // run_one() on a context with no outstanding work returns 0 at once,
    // taking the idle early-out before the event loop.
    void testRunOneOnIdleReturnsZero()
    {
        io_context ioc(Backend);
        BOOST_TEST(ioc.run_one() == 0);
    }

    void run()
    {
        testRunForLeavesParkedReadParked();
        testRunOneForLeavesParkedReadParked();
        testRunForFiresArmedDelay();
        testPollLeavesParkedReadParked();
        testTwoThreadBoundedFollower();
        testOversizeBudgetThrows();
        testRunOneOnIdleReturnsZero();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(bounded_run_test, "boost.corosio.bounded_run")

} // namespace boost::corosio

#endif
