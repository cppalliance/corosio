//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_timer.hpp>
#include <boost/corosio/native/native_io_context.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_timer_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_timer<Backend>&>().wait()),
            decltype(std::declval<io_timer&>().wait())>,
        "native_timer::wait must shadow io_timer::wait");

    void testTimerConstruct()
    {
        io_context ctx(Backend);
        native_timer<Backend> t(ctx);
        BOOST_TEST_PASS();
    }

    void testTimerConstructDuration()
    {
        io_context ctx(Backend);
        native_timer<Backend> t(ctx, std::chrono::milliseconds(100));
        BOOST_TEST(t.expiry() > timer::time_point{});
    }

    void testTimerWait()
    {
        io_context ctx(Backend);
        native_timer<Backend> t(ctx);
        t.expires_after(std::chrono::milliseconds(10));

        bool done = false;
        std::error_code result_ec;

        auto task = [](native_timer<Backend>& t_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ctx.get_executor())(task(t, result_ec, done));

        ctx.run();
        BOOST_TEST(done);
        BOOST_TEST(!result_ec);
    }

    void testTimerWaitExpired()
    {
        io_context ctx(Backend);
        native_timer<Backend> t(ctx);
        t.expires_at(timer::clock_type::now() - std::chrono::seconds(1));

        bool done = false;
        std::error_code result_ec;

        auto task = [](native_timer<Backend>& t_ref, std::error_code& ec_out,
                       bool& done_out) -> capy::task<> {
            auto [ec] = co_await t_ref.wait();
            ec_out    = ec;
            done_out  = true;
        };
        capy::run_async(ctx.get_executor())(task(t, result_ec, done));

        ctx.run();
        BOOST_TEST(done);
        BOOST_TEST(!result_ec);
    }

    void testTimerPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_timer<Backend> nt(ctx);
        nt.expires_after(std::chrono::milliseconds(100));

        timer& base = nt;
        BOOST_TEST(base.expiry() == nt.expiry());

        io_timer& io_base = nt;
        BOOST_TEST(io_base.expiry() == nt.expiry());
    }

    void run()
    {
        testTimerConstruct();
        testTimerConstructDuration();
        testTimerWait();
        testTimerWaitExpired();
        testTimerPolymorphicSlice();
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_timer_test_epoll : native_timer_test<epoll>
{};
TEST_SUITE(native_timer_test_epoll, "boost.corosio.native.timer.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_timer_test_select : native_timer_test<select>
{};
TEST_SUITE(native_timer_test_select, "boost.corosio.native.timer.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_timer_test_kqueue : native_timer_test<kqueue>
{};
TEST_SUITE(native_timer_test_kqueue, "boost.corosio.native.timer.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_timer_test_iocp : native_timer_test<iocp>
{};
TEST_SUITE(native_timer_test_iocp, "boost.corosio.native.timer.iocp");
#endif

} // namespace boost::corosio
