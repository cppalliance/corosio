//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_io_context.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>

#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_io_context_test
{
    void testIoContextConstruct()
    {
        native_io_context<Backend> ctx;
        BOOST_TEST_PASS();
    }

    void testIoContextConstructHint()
    {
        native_io_context<Backend> ctx(1);
        BOOST_TEST_PASS();
    }

    void testIoContextPolymorphicSlice()
    {
        native_io_context<Backend> ctx;
        [[maybe_unused]] io_context& base = ctx;
        BOOST_TEST_PASS();
    }

    void testIoContextPoll()
    {
        native_io_context<Backend> ctx;
        auto ex = ctx.get_executor();

        bool done = false;
        auto task = [](bool& done_out) -> capy::task<> {
            done_out = true;
            co_return;
        };
        capy::run_async(ex)(task(done));

        auto n = ctx.poll();
        BOOST_TEST(n > 0u);
        BOOST_TEST(done);
    }

    void testIoContextStopRestart()
    {
        native_io_context<Backend> ctx;
        BOOST_TEST(!ctx.stopped());
        ctx.stop();
        BOOST_TEST(ctx.stopped());
        ctx.restart();
        BOOST_TEST(!ctx.stopped());
    }

    void testIoContextRunFor()
    {
        native_io_context<Backend> ctx;
        auto ex = ctx.get_executor();

        bool done = false;
        auto task = [](bool& done_out) -> capy::task<> {
            done_out = true;
            co_return;
        };
        capy::run_async(ex)(task(done));

        auto n = ctx.run_for(std::chrono::seconds(30));
        BOOST_TEST(n > 0u);
        BOOST_TEST(done);
    }

    void testIoContextConstructWithOptions()
    {
        io_context_options opts;
        opts.max_events_per_poll   = 64;
        opts.inline_budget_initial = 4;
        opts.inline_budget_max     = 16;
        opts.unassisted_budget     = 4;

        native_io_context<Backend> ctx(opts, 2);
        BOOST_TEST(!ctx.stopped());
    }

    void testIoContextRun()
    {
        native_io_context<Backend> ctx;
        auto ex = ctx.get_executor();

        bool done = false;
        auto task = [](bool& done_out) -> capy::task<> {
            done_out = true;
            co_return;
        };
        capy::run_async(ex)(task(done));

        auto n = ctx.run();
        BOOST_TEST(n > 0u);
        BOOST_TEST(done);
    }

    // Exercises the `rel_time > 1s` clamp branch in
    // native_io_context::run_one_until. With no outstanding work, the
    // scheduler stops immediately and returns 0 — the loop still entered
    // with rel_time > 1s, hitting the clamp.
    void testIoContextRunOneUntilLongDeadlineNoWork()
    {
        native_io_context<Backend> ctx;
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto n = ctx.run_one_until(deadline);
        BOOST_TEST(n == 0u);
        BOOST_TEST(ctx.stopped());
    }

    // Exercises the post-loop `return 0` after run_one_until times out.
    // Outstanding work keeps the scheduler alive; the inner wait_one
    // returns 0 each iteration until the deadline passes.
    void testIoContextRunForWithOutstandingWork()
    {
        native_io_context<Backend> ctx;
        auto ex = ctx.get_executor();

        ex.on_work_started();

        // Outstanding work keeps the scheduler alive; run_for must
        // return when its timeout elapses rather than block forever. A
        // genuine hang surfaces as a harness timeout, not a wall-clock
        // assert.
        std::size_t n = ctx.run_for(std::chrono::milliseconds(50));
        BOOST_TEST(n == 0u);

        ex.on_work_finished();
    }

    // A deadline that has already passed runs nothing and leaves the
    // context as it was.
    void testIoContextExpiredDeadlineRunsNothing()
    {
        native_io_context<Backend> ctx;
        auto const past =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        auto const late = std::chrono::milliseconds(-1);
        BOOST_TEST(ctx.run_one_until(past) == 0u);
        BOOST_TEST(ctx.run_until(past) == 0u);
        BOOST_TEST(ctx.run_one_for(late) == 0u);
        BOOST_TEST(ctx.run_for(late) == 0u);
        BOOST_TEST(!ctx.stopped());

        bool done = false;
        auto task = [](bool& done_out) -> capy::task<> {
            done_out = true;
            co_return;
        };
        capy::run_async(ctx.get_executor())(task(done));
        BOOST_TEST(ctx.run_one_until(past) == 0u);
        BOOST_TEST(ctx.run_until(past) == 0u);
        BOOST_TEST(ctx.run_one_for(late) == 0u);
        BOOST_TEST(ctx.run_for(late) == 0u);
        BOOST_TEST(!done);
        BOOST_TEST(!ctx.stopped());

        BOOST_TEST(ctx.poll() == 1u);
        BOOST_TEST(done);
    }

    void run()
    {
        testIoContextConstruct();
        testIoContextConstructHint();
        testIoContextConstructWithOptions();
        testIoContextPolymorphicSlice();
        testIoContextPoll();
        testIoContextStopRestart();
        testIoContextRun();
        testIoContextRunFor();
        testIoContextRunOneUntilLongDeadlineNoWork();
        testIoContextExpiredDeadlineRunsNothing();
        testIoContextRunForWithOutstandingWork();
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_io_context_test_epoll : native_io_context_test<epoll>
{};
TEST_SUITE(
    native_io_context_test_epoll, "boost.corosio.native.io_context.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_io_context_test_select : native_io_context_test<select>
{};
TEST_SUITE(
    native_io_context_test_select, "boost.corosio.native.io_context.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_io_context_test_kqueue : native_io_context_test<kqueue>
{};
TEST_SUITE(
    native_io_context_test_kqueue, "boost.corosio.native.io_context.kqueue");
#endif

#if BOOST_COROSIO_HAS_IOCP
struct native_io_context_test_iocp : native_io_context_test<iocp>
{};
TEST_SUITE(native_io_context_test_iocp, "boost.corosio.native.io_context.iocp");
#endif

#if BOOST_COROSIO_HAS_URING
struct native_io_context_test_uring : native_io_context_test<uring>
{};
TEST_SUITE(
    native_io_context_test_uring, "boost.corosio.native.io_context.uring");
#endif

} // namespace boost::corosio
