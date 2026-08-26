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
#include <system_error>
#include <tuple>

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

    void testSignalReaderRegisterFails()
    {
        in_child([]{
            io_context ioc(epoll);
            signal_set ss(ioc);
            std::error_code ec;
            bool fired = false;
            {
                fault_scope f(sys::epoll_ctl, ENOMEM);
                ec = ss.add(SIGUSR2);
                fired = f.fired();
            }
            // Not latched: the next add retries the registration.
            return fired && ec == std::errc::not_enough_memory &&
                !ss.add(SIGUSR2) && !ss.clear();
        });
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
        testSignalReaderRegisterFails();
    }
};

TEST_SUITE(epoll_faults, "boost.corosio.fault.epoll");

} // boost::corosio::test::fault

#endif
