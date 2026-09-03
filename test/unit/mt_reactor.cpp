//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reactor-scheduler paths that need batched or contended dispatch: a
// descriptor event completing several parked ops at once, and a
// cond-parked follower woken by work posted from a foreign thread.
// Durations only bound blocking; nothing asserts elapsed time.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || \
    BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct mt_reactor_test
{
    void
    testEventCompletesBatchedOps()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // A read and a readiness wait park on the same descriptor, so
        // one readable event dispatches both in a single batch. Two
        // bytes arrive and the read takes one, so readiness survives
        // the read and the wait can complete.
        char buf[1];
        std::error_code rec, wec;
        int done    = 0;
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wec       = ec;
            ++done;
        };
        auto trip = [&]() -> capy::task<> {
            char cc[2] = {'z', 'z'};
            std::ignore = ::send(
                static_cast<int>(s2.native_handle()), cc, 2, MSG_NOSIGNAL);
            co_return;
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(trip());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(!rec);
        BOOST_TEST(!wec);
    }

    void
    testForeignPostWakesParkedFollower()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // The parked read keeps outstanding work alive so both runner
        // threads stay inside the scheduler: one leads in the reactor,
        // the other parks in the signal wait. Each foreign post then
        // has a parked follower to wake.
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
        auto slice = [&] {
            entered.fetch_add(1);
            for (int i = 0; i < 100 && nops.load() < 20; ++i)
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(10));
        };
        std::thread ra(slice), rb(slice);

        while (entered.load() < 2)
        {
        }
        for (int i = 0; i < 20; ++i)
        {
            // The counter travels as a parameter: a loop-scoped
            // closure would die before the runner threads execute the
            // frame that references it.
            capy::run_async(ex)([](std::atomic<int>* n) -> capy::task<> {
                n->fetch_add(1);
                co_return;
            }(&nops));
        }
        ra.join();
        rb.join();
        BOOST_TEST_EQ(nops.load(), 20);

        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST(resumed);
    }

    void
    run()
    {
        testEventCompletesBatchedOps();
        testForeignPostWakesParkedFollower();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(mt_reactor_test, "boost.corosio.mt_reactor")

} // namespace boost::corosio

#endif
