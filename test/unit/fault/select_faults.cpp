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
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <chrono>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_HAS_SELECT

namespace boost::corosio::test::fault {

namespace {

endpoint loopback()
{
    return endpoint(ipv4_address::loopback(), 0);
}

} // namespace

struct select_faults
{
    void testConstructorFails()
    {
        {
            int before = open_fds();
            fault_scope f(sys::pipe, EMFILE);
            expect_system_error([]{ io_context ioc(select); },
                std::errc::too_many_files_open);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
        // Three fcntl calls configure each end of the interrupt pipe,
        // read end first: 1-3 fail the read end, 4-6 the write end.
        for(unsigned nth : {1u, 2u, 3u, 4u, 5u, 6u})
        {
            int before = open_fds();
            fault_scope f(sys::fcntl, EINVAL, nth);
            expect_system_error([]{ io_context ioc(select); },
                std::errc::invalid_argument);
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    void testOpenFcntlFails()
    {
        io_context ioc(select);
        for(unsigned nth : {1u, 2u, 3u})
        {
            int before = open_fds();
            tcp_socket s(ioc);
            fault_scope f(sys::fcntl, EINVAL, nth);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST(!s.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
        for(unsigned nth : {1u, 2u, 3u})
        {
            int before = open_fds();
            tcp_acceptor acc(ioc);
            fault_scope f(sys::fcntl, EINVAL, nth);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST(ec == std::errc::invalid_argument);
            BOOST_TEST(!acc.is_open());
            BOOST_TEST_EQ(open_fds(), before);
        }
    }

    void testAcceptFails()
    {
        io_context ioc(select);
        tcp_acceptor acc(ioc, loopback());
        std::error_code aec;
        int leaked = 0;
        auto body = [&]() -> capy::task<>
        {
            tcp_socket client(ioc), server(ioc);
            {
                auto [ec] = co_await client.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            {
                // EINTR is retried inside accept_policy, then the real
                // accept succeeds.
                fault_scope f(sys::accept, EINTR);
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
                int before = open_fds();
                fault_scope f(sys::accept, ECONNABORTED);
                auto [ec] = co_await acc.accept(server);
                aec = ec;
                leaked = open_fds() - before;
                BOOST_TEST(f.fired());
                BOOST_TEST(!server.is_open());
            }
            // The aborted accept did not consume the pending
            // connection, so the retry still yields it.
            {
                auto [ec] = co_await acc.accept(server);
                BOOST_TEST(!ec);
            }
            server.close();
            client2.close();
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST(aec == std::errc::connection_aborted);
        BOOST_TEST_EQ(leaked, 0);
    }

    void testAcceptFcntlFails()
    {
        io_context ioc(select);
        tcp_acceptor acc(ioc, loopback());
        std::error_code ecs[3];
        int leaked[3] = {};
        auto body = [&]() -> capy::task<>
        {
            for(unsigned nth = 1; nth <= 3; ++nth)
            {
                tcp_socket c(ioc), s(ioc);
                {
                    auto [ec] = co_await c.connect(acc.local_endpoint());
                    BOOST_TEST(!ec);
                }
                int before = open_fds();
                // Armed after the connect, so the count starts at the
                // three fcntl calls accept_policy makes on the
                // accepted descriptor.
                fault_scope f(sys::fcntl, EINVAL, nth);
                auto [ec] = co_await acc.accept(s);
                ecs[nth - 1] = ec;
                leaked[nth - 1] = open_fds() - before;
                BOOST_TEST(f.fired());
                BOOST_TEST(!s.is_open());
                c.close();
            }
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        for(unsigned i = 0; i < 3; ++i)
        {
            BOOST_TEST(ecs[i] == std::errc::invalid_argument);
            BOOST_TEST_EQ(leaked[i], 0);
        }
    }

    void testRunLoopFaults()
    {
        io_context ioc(select);
        // EINTR and EBADF are the two codes select() retries; every
        // other one leaves the run loop through an exception. Only the
        // exceptional branch is driven from a test: arming either
        // retried code aborts the process instead of looping, so it is
        // left uncovered rather than asserted.
        fault_scope f(sys::select, EINVAL);
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

    void run()
    {
        if(skip_under_valgrind())
            return;
        testConstructorFails();
        testOpenFcntlFails();
        testAcceptFails();
        testAcceptFcntlFails();
        testRunLoopFaults();
    }
};

TEST_SUITE(select_faults, "boost.corosio.fault.select");

} // boost::corosio::test::fault

#endif
