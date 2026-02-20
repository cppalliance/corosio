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

#include "context.hpp"
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
        io_context& base = ctx;
        (void)base;
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

        auto n = ctx.run_for(std::chrono::milliseconds(100));
        BOOST_TEST(n > 0u);
        BOOST_TEST(done);
    }

    void run()
    {
        testIoContextConstruct();
        testIoContextConstructHint();
        testIoContextPolymorphicSlice();
        testIoContextPoll();
        testIoContextStopRestart();
        testIoContextRunFor();
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

} // namespace boost::corosio
