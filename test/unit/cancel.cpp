//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/cancel.hpp>

#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <stop_token>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct cancel_test
{
    void testTimeoutFires()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(10));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testInnerCompletesFirst()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::milliseconds(10));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testZeroTimeout()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(0));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testParentCancellation()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);
        timer delay(ioc);

        std::stop_source stop_src;
        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::seconds(60));
            result_ec = ec;
            completed = true;
        };

        auto canceller = [&]() -> capy::task<> {
            delay.expires_after(std::chrono::milliseconds(10));
            (void)co_await delay.wait();
            stop_src.request_stop();
        };

        capy::run_async(ioc.get_executor(), stop_src.get_token())(
            task());
        capy::run_async(ioc.get_executor())(canceller());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testAlreadyExpiredDeadline()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_at(
                inner_timer.wait(), timeout_timer,
                timer::clock_type::now() - std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testCancelAt()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));
        auto deadline =
            timer::clock_type::now() + std::chrono::milliseconds(10);

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_at(
                inner_timer.wait(), timeout_timer, deadline);
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testTimerReuse()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        int completed = 0;

        auto task = [&]() -> capy::task<> {
            // First: inner completes before timeout
            inner_timer.expires_after(std::chrono::milliseconds(10));
            auto [ec1] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::seconds(1));
            BOOST_TEST(!ec1);
            ++completed;

            // Second: timeout fires
            inner_timer.expires_after(std::chrono::seconds(60));
            auto [ec2] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(10));
            BOOST_TEST(ec2 == capy::cond::canceled);
            ++completed;

            // Third: inner completes again
            inner_timer.expires_after(std::chrono::milliseconds(10));
            auto [ec3] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::seconds(1));
            BOOST_TEST(!ec3);
            ++completed;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST_EQ(completed, 3);
    }

    // -- Convenience overloads (no user-supplied timer) --

    void testConvenienceTimeoutFires()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(),
                std::chrono::milliseconds(10));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testConvenienceInnerCompletesFirst()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::milliseconds(10));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(),
                std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testConvenienceCancelAt()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));
        auto deadline =
            timer::clock_type::now() + std::chrono::milliseconds(10);

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_at(
                inner_timer.wait(), deadline);
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void run()
    {
        testTimeoutFires();
        testInnerCompletesFirst();
        testZeroTimeout();
        testParentCancellation();
        testAlreadyExpiredDeadline();
        testCancelAt();
        testTimerReuse();
        testConvenienceTimeoutFires();
        testConvenienceInnerCompletesFirst();
        testConvenienceCancelAt();
    }
};

COROSIO_BACKEND_TESTS(cancel_test, "boost.corosio.cancel")

} // namespace boost::corosio
