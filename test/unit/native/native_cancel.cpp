//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/native/native_cancel.hpp>

#include <boost/corosio/timer.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_cancel_test
{
    void testConvenienceTimeoutFires()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after<Backend>(
                inner_timer.wait(), std::chrono::milliseconds(10));
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
            auto [ec] = co_await cancel_after<Backend>(
                inner_timer.wait(), std::chrono::seconds(1));
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
            auto [ec] =
                co_await cancel_at<Backend>(inner_timer.wait(), deadline);
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
        testConvenienceTimeoutFires();
        testConvenienceInnerCompletesFirst();
        testConvenienceCancelAt();
    }
};

COROSIO_BACKEND_TESTS(native_cancel_test, "boost.corosio.native_cancel")

} // namespace boost::corosio
