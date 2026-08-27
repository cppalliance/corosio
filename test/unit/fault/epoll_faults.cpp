//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "fault.hpp"
#include "fault_test_utils.hpp"
#include "context.hpp"
#include "test_suite.hpp"
#include "test_utils.hpp"

#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <stop_token>
#include <system_error>
#include <tuple>

#include <netinet/in.h>
#include <sys/socket.h>

#if BOOST_COROSIO_HAS_EPOLL

namespace boost::corosio::test::fault {

namespace {

endpoint loopback()
{
    return endpoint(ipv4_address::loopback(), 0);
}

} // namespace

struct epoll_faults
{
    void testConstructorFails()
    {
        auto expect_throw = [](sys s, unsigned nth, int err, std::errc code)
        {
            fault_scope f(s, err, nth);
            expect_system_error([&]{ io_context ioc(epoll); }, code);
            BOOST_TEST(f.fired());
        };
        expect_throw(sys::epoll_create1, 1, EMFILE,
            std::errc::too_many_files_open);
        expect_throw(sys::eventfd, 1, EMFILE,
            std::errc::too_many_files_open);
        expect_throw(sys::timerfd_create, 1, EMFILE,
            std::errc::too_many_files_open);
        // 1 registers the eventfd, 2 the timerfd.
        expect_throw(sys::epoll_ctl, 1, ENOMEM,
            std::errc::not_enough_memory);
        expect_throw(sys::epoll_ctl, 2, ENOMEM,
            std::errc::not_enough_memory);
    }

    void testOpenFails()
    {
        io_context ioc(epoll);
        {
            tcp_socket s(ioc);
            fault_scope f(sys::socket, EMFILE);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!s.is_open());
        }
        {
            // The socket already exists when registration fails, so the
            // failure path owns closing it.
            int before = open_fds();
            tcp_socket s(ioc);
            fault_scope f(sys::epoll_ctl, ENOMEM);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::not_enough_memory);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    void testAcceptorRegisterFails()
    {
        io_context ioc(epoll);
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(loopback()));
        // listen is where an acceptor first reaches register_descriptor.
        fault_scope f(sys::epoll_ctl, ENOMEM);
        BOOST_TEST(acc.listen() == std::errc::not_enough_memory);
        BOOST_TEST(f.fired());
        // Not latched: the descriptor is still unregistered, so a
        // second listen registers it.
        BOOST_TEST(!acc.listen());
    }

    void testAcceptFails()
    {
        io_context ioc(epoll);
        tcp_acceptor acc(ioc, loopback());
        tcp_socket client(ioc), server(ioc);
        std::error_code aec, aec2;
        auto body = [&]() -> capy::task<>
        {
            {
                auto [ec] = co_await client.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                // EINTR is retried inside accept_policy, then the real
                // accept4 succeeds.
                fault_scope f(sys::accept4, EINTR);
                auto [ec] = co_await acc.accept(server);
                BOOST_TEST(f.fired());
                BOOST_TEST(!ec);
            }
            server.close();
            client.close();
            tcp_socket client2(ioc);
            {
                auto [ec] = co_await client2.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                fault_scope f(sys::accept4, ECONNABORTED);
                auto [ec] = co_await acc.accept(server);
                aec = ec;
                BOOST_TEST(f.fired());
            }
            {
                auto [ec] = co_await acc.accept(server);
                BOOST_TEST(!ec);
            }
            server.close();
            client2.close();
            tcp_socket client3(ioc);
            {
                auto [ec] = co_await client3.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                // The accepted fd fails to register: the impl is
                // destroyed, which closes it, and the error is reported.
                int before = open_fds();
                fault_scope f(sys::epoll_ctl, ENOMEM);
                auto [ec] = co_await acc.accept(server);
                aec2 = ec;
                BOOST_TEST(f.fired());
                BOOST_TEST_EQ(open_fds(), before);
            }
            client3.close();
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(aec == std::errc::connection_aborted);
        BOOST_TEST(aec2 == std::errc::not_enough_memory);
        BOOST_TEST(!server.is_open());
    }

    void testRunLoopFaults()
    {
        {
            io_context ioc(epoll);
            fault_scope f(sys::epoll_wait, EINTR);
            bool done = false;
            auto body = [&]() -> capy::task<>
            {
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
                done = true;
            };
            capy::run_async(ioc.get_executor())(body());
            ioc.run();
            BOOST_TEST(f.fired());
            BOOST_TEST(done);
        }
        {
            io_context ioc(epoll);
            fault_scope f(sys::epoll_wait, EBADF);
            auto body = [&]() -> capy::task<>
            {
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
            };
            capy::run_async(ioc.get_executor())(body());
            expect_system_error([&]{ ioc.run(); },
                std::errc::bad_file_descriptor);
            BOOST_TEST(f.fired());
        }
        {
            io_context ioc(epoll);
            fault_scope f(sys::timerfd_settime, EINVAL);
            auto body = [&]() -> capy::task<>
            {
                std::ignore = co_await corosio::delay(
                    std::chrono::milliseconds(1));
            };
            capy::run_async(ioc.get_executor())(body());
            expect_system_error([&]{ ioc.run(); },
                std::errc::invalid_argument);
            BOOST_TEST(f.fired());
        }
    }

    /* A wake that never left has to be retried, not swallowed.

       `eventfd_armed_` is set before the write, so it stands for a byte
       in the eventfd. A write that failed put none there: leaving the
       flag set would coalesce every later interrupt into a wake that
       does not exist, and a wait only an interrupt could end would
       never end. The second arm is the observable — it can only fire if
       the second interrupt actually reached `write`.
    */
    void testInterruptWriteFails()
    {
        io_context ioc(epoll);
        {
            // stop() is the one interrupt that reaches the eventfd
            // without a reactor thread (reactor_scheduler::stop), and
            // it interrupts only on the transition, so each stop below
            // is one write.
            fault_scope first(sys::write, EIO, 1);
            fault_scope second(sys::write, EIO, 2);
            ioc.stop();
            BOOST_TEST(first.fired());
            BOOST_TEST(!second.fired());
            ioc.restart();
            ioc.stop();
            BOOST_TEST(second.fired());
        }
        // And the loop the failed wakes were aimed at still runs: timed
        // work fires and a stop from inside ends run().
        ioc.restart();
        bool done = false;
        auto body = [&]() -> capy::task<>
        {
            std::ignore = co_await corosio::delay(
                std::chrono::milliseconds(1));
            done = true;
            ioc.stop();
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(done);
    }

    /* Adoption is the acceptor's other way into the reactor.

       `listen()` registers a descriptor the library made; `assign()`
       registers one the caller made, and only that path leaves the
       caller still owning it when the reactor refuses. The acceptor
       has to come back closed with the descriptor untouched.
    */
    void testAcceptorAssignRegisterFails()
    {
        io_context ioc(epoll);
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(static_cast<int>(h) >= 0);
        make_native_adoptable(h);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(::bind(static_cast<int>(h),
            reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
        BOOST_TEST_EQ(::listen(static_cast<int>(h), 1), 0);

        int const before = open_fds();
        tcp_acceptor acc(ioc);
        {
            fault_scope f(sys::epoll_ctl, ENOMEM);
            auto ec = acc.assign(h);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), 1u);
            BOOST_TEST(ec == std::errc::not_enough_memory);
        }
        BOOST_TEST(!acc.is_open());
        BOOST_TEST(native_socket_valid(h));
        BOOST_TEST_EQ(open_fds(), before);
        // Not latched: the descriptor is still unregistered.
        BOOST_TEST(!acc.assign(h));
        acc.close();
    }

    /* The accept that the reactor dispatches, rather than the one the
       initiator speculates on.

       A first accept4 with no peer waiting reports EAGAIN and parks the
       op, so the second call is the reactor's retry — a different code
       path with its own error and completion arms, and the one an
       accepted descriptor is registered from.
    */
    void testPostedAcceptFails()
    {
        io_context ioc(epoll);
        tcp_acceptor acc(ioc, loopback());
        tcp_socket client(ioc), server(ioc);
        // Opened before any arm so its own registration is not counted.
        BOOST_TEST(!client.open(tcp::v4()));
        std::error_code aec;
        std::stop_source guard;
        auto accept_body = [&]() -> capy::task<>
        {
            {
                // 1 is the speculative accept, which finds nothing.
                fault_scope f(sys::accept4, EMFILE, 2);
                auto [ec] = co_await acc.accept(server);
                aec = ec;
                BOOST_TEST(f.fired());
            }
            // A refused accept dequeues nothing, so the acceptor is
            // still open and the connection still pending.
            auto [ec] = co_await acc.accept(server);
            BOOST_TEST(!ec);
            guard.request_stop();
        };
        auto connect_body = [&]() -> capy::task<>
        {
            auto [ec] = co_await client.connect(acc.local_endpoint());
            BOOST_TEST(!ec);
        };
        // The second accept only resolves if the refused one left the
        // pending connection alone; a run loop that never returns
        // would otherwise read as a job timeout rather than a failure.
        bool expired = false;
        capy::run_async(ioc.get_executor())(accept_body());
        capy::run_async(ioc.get_executor())(connect_body());
        capy::run_async(ioc.get_executor(), guard.get_token())(
            stop_guard(ioc, expired));
        ioc.run();
        BOOST_TEST(!expired);
        BOOST_TEST(aec == std::errc::too_many_files_open);
        BOOST_TEST(server.is_open());
    }

    void testPostedAcceptRegisterFails()
    {
        io_context ioc(epoll);
        tcp_acceptor acc(ioc, loopback());
        tcp_socket client(ioc), server(ioc);
        BOOST_TEST(!client.open(tcp::v4()));
        std::error_code aec;
        int leaked = 0;
        auto accept_body = [&]() -> capy::task<>
        {
            int const before = open_fds();
            fault_scope f(sys::epoll_ctl, ENOMEM);
            auto [ec] = co_await acc.accept(server);
            aec = ec;
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), 1u);
            // The peer implementation owns the accepted descriptor by
            // then, so destroying it is what closes it.
            leaked = open_fds() - before;
        };
        auto connect_body = [&]() -> capy::task<>
        {
            auto [ec] = co_await client.connect(acc.local_endpoint());
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(accept_body());
        capy::run_async(ioc.get_executor())(connect_body());
        ioc.run();
        BOOST_TEST(aec == std::errc::not_enough_memory);
        BOOST_TEST(!server.is_open());
        BOOST_TEST_EQ(leaked, 0);
    }

    void run()
    {
        if(skip_under_valgrind())
            return;
        testConstructorFails();
        testOpenFails();
        testAcceptorRegisterFails();
        testAcceptFails();
        testRunLoopFaults();
        testInterruptWriteFails();
        testAcceptorAssignRegisterFails();
        testPostedAcceptFails();
        testPostedAcceptRegisterFails();
    }
};

TEST_SUITE(epoll_faults, "boost.corosio.fault.epoll");

} // boost::corosio::test::fault

#endif
