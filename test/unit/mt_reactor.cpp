//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
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

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct mt_reactor_test
{
    void testEventCompletesBatchedOps()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
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
            char cc[2]  = {'z', 'z'};
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

    void testForeignPostWakesParkedFollower()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
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

        // Yield while spinning: under Valgrind's serialized scheduler
        // a tight spin starves the freshly created runner threads of
        // their first slice, and the wait never completes.
        while (entered.load() < 2)
            std::this_thread::yield();
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

    // An acceptor wait started off the reactor thread while that thread
    // blocks in the reactor. Select watches an fd only while an op is
    // parked on it, so the park has to wake the blocked reactor.
    void testAcceptorWaitWakesBlockedReactor()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto const port = acc.local_endpoint().port();

        // The parked read keeps run() inside the reactor.
        char buf[1];
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = ec;
            std::ignore = n;
        };
        capy::run_async(ex)(reader());
        std::thread runner([&] { ioc.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::atomic<bool> done{false};
        std::error_code wec;
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            done.store(true);
        };
        // Runs the waiter on this thread up to the parked wait.
        capy::io_env env{ex, std::stop_token{}, nullptr};
        std::optional<capy::task<>> parked;
        parked.emplace(waiter());
        parked->await_suspend(std::noop_coroutine(), &env).resume();

        int client = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(client >= 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(
            ::connect(client, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)),
            0);

        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        bool const woke = done.load();

        ioc.stop();
        runner.join();
        ::close(client);
        BOOST_TEST(woke);
        BOOST_TEST(!wec);

        // Drain the parked read, and the wait if it never woke.
        acc.cancel();
        s1.cancel();
        ioc.restart();
        ioc.run();
    }

    void run()
    {
        testEventCompletesBatchedOps();
        testForeignPostWakesParkedFollower();
        testAcceptorWaitWakesBlockedReactor();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(mt_reactor_test, "boost.corosio.mt_reactor")

} // namespace boost::corosio

#endif
