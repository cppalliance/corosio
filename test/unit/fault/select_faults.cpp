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
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cerrno>
#include <chrono>
#include <system_error>
#include <tuple>

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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
        // EINTR and EBADF are the two codes select() retries: the poll
        // is abandoned for this round and the run loop keeps going, so
        // the delay armed before it still fires.
        for(int err : {EINTR, EBADF})
        {
            io_context ioc(select);
            fault_scope f(sys::select, err);
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
        // Every other code leaves the run loop through an exception.
        {
            io_context ioc(select);
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
    }

    /* This backend coalesces nothing: every interrupt writes its own
       byte to the self-pipe, so a failed write costs exactly that one
       interrupt. The arms below are what holds that shape in place —
       the eventfd and kqueue backends reach it by clearing a flag, and
       a flag introduced here would have to clear one too.
    */
    void testInterruptWriteFails()
    {
        io_context ioc(select);
        {
            // stop() is the one interrupt that reaches the self-pipe
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

    /* Descriptors this backend cannot represent.

       select() addresses descriptors by bit position in an fd_set, so
       a number at or above FD_SETSIZE has nowhere to go and FD_SET on
       it would write past the set. Every entry point that can be
       handed such a number rejects it instead, and the rejection is
       what these three tests drive: adoption (validate_assigned_fd),
       creation (set_fd_options) and acceptance (accept_policy).
    */
    void testAssignAboveFdSetsize()
    {
        io_context ioc(select);
        int const before = open_fds();
        auto h = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(static_cast<int>(h) >= 0);
        make_native_adoptable(h);
        int const high = dup_above_fd_setsize(static_cast<int>(h));
        if(high < 0)
        {
            skip_no_high_fd("testAssignAboveFdSetsize");
            close_native_socket(h);
            return;
        }
        {
            tcp_socket s(ioc);
            auto ec = s.assign(static_cast<native_handle_type>(high));
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!s.is_open());
        }
        {
            tcp_acceptor acc(ioc);
            auto ec = acc.assign(static_cast<native_handle_type>(high));
            BOOST_TEST(ec == std::errc::too_many_files_open);
            BOOST_TEST(!acc.is_open());
        }
        // The rejection is non-mutating: the caller still owns both.
        BOOST_TEST(native_socket_valid(
            static_cast<native_handle_type>(high)));
        BOOST_TEST(native_socket_valid(h));
        ::close(high);
        close_native_socket(h);
        BOOST_TEST_EQ(open_fds(), before);
    }

    void testOpenAboveFdSetsize()
    {
        // The context first: the scheduler's own self-pipe has to be
        // representable, and the wall would deny it a number.
        io_context ioc(select);
        fd_wall wall;
        if(!wall.ok())
        {
            skip_no_high_fd("testOpenAboveFdSetsize");
            return;
        }
        tcp_socket s(ioc);
        auto ec = s.open(tcp::v4());
        BOOST_TEST(ec == std::errc::too_many_files_open);
        BOOST_TEST(!s.is_open());
        tcp_acceptor acc(ioc);
        BOOST_TEST(acc.open() == std::errc::too_many_files_open);
        BOOST_TEST(!acc.is_open());
    }

    void testAcceptAboveFdSetsize()
    {
        io_context ioc(select);
        tcp_acceptor acc(ioc, loopback());
        tcp_socket client(ioc), server(ioc);
        std::error_code aec;
        bool skipped = false;
        int leaked = 0;
        auto body = [&]() -> capy::task<>
        {
            {
                auto [ec] = co_await client.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            fd_wall wall;
            if(!wall.ok())
            {
                skipped = true;
                co_return;
            }
            int const before = open_fds();
            auto [ec] = co_await acc.accept(server);
            // accept_policy closes the descriptor it cannot represent
            // before reporting, so the pending connection is consumed
            // and nothing is left behind.
            leaked = open_fds() - before;
            aec = ec;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        if(skipped)
        {
            skip_no_high_fd("testAcceptAboveFdSetsize");
            return;
        }
        BOOST_TEST(aec == std::errc::invalid_argument);
        BOOST_TEST(!server.is_open());
        BOOST_TEST_EQ(leaked, 0);
    }

#ifdef SO_NOSIGPIPE
    /* The per-descriptor SIGPIPE guard, where the platform has one.

       This backend is portable rather than Linux-shaped: its write
       policy falls back to write(), which carries no per-call flag, so
       on a platform that defines SO_NOSIGPIPE the socket-level flag is
       the only guard there is and select_traits::set_fd_options
       treats a refusal as fatal. Linux has no such option and compiles
       both arms away.
    */
    void testOpenNoSigPipeFails()
    {
        io_context ioc(select);
        int const before = open_fds();
        {
            // SO_NOSIGPIPE is the only setsockopt an AF_INET open
            // makes, so the first call is that one.
            tcp_socket s(ioc);
            fault_scope f(sys::setsockopt, ENOPROTOOPT);
            auto ec = s.open(tcp::v4());
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), 1u);
            BOOST_TEST(ec == std::errc::no_protocol_option);
            BOOST_TEST(!s.is_open());
        }
        {
            tcp_acceptor acc(ioc);
            fault_scope f(sys::setsockopt, ENOPROTOOPT);
            auto ec = acc.open();
            BOOST_TEST(f.fired());
            BOOST_TEST_EQ(f.count(), 1u);
            BOOST_TEST(ec == std::errc::no_protocol_option);
            BOOST_TEST(!acc.is_open());
        }
        // The descriptor exists before the option is refused, so the
        // failure path owns closing it.
        BOOST_TEST_EQ(open_fds(), before);
    }

    void testAcceptNoSigPipeFails()
    {
        io_context ioc(select);
        tcp_acceptor acc(ioc, loopback());
        std::error_code aec;
        int leaked = 0;
        unsigned calls = 0;
        auto body = [&]() -> capy::task<>
        {
            tcp_socket c(ioc), s(ioc);
            {
                auto [ec] = co_await c.connect(acc.local_endpoint());
                BOOST_TEST(!ec);
            }
            int const before = open_fds();
            // Armed after the connect, so the first setsockopt is the
            // one accept_policy makes on the accepted descriptor
            // (accept_policy::do_accept).
            fault_scope f(sys::setsockopt, ENOPROTOOPT);
            auto [ec] = co_await acc.accept(s);
            aec = ec;
            calls = f.count();
            leaked = open_fds() - before;
            BOOST_TEST(f.fired());
            BOOST_TEST(!s.is_open());
            c.close();
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();
        BOOST_TEST_EQ(calls, 1u);
        BOOST_TEST(aec == std::errc::no_protocol_option);
        // accept_policy closes the descriptor it could not guard, and
        // the errno it preserves is the option's, not close()'s.
        BOOST_TEST_EQ(leaked, 0);
    }
#endif

    void run()
    {
        if(skip_under_valgrind())
            return;
        testConstructorFails();
        testOpenFcntlFails();
        testAcceptFails();
        testAcceptFcntlFails();
#ifdef SO_NOSIGPIPE
        testOpenNoSigPipeFails();
        testAcceptNoSigPipeFails();
#endif
        testRunLoopFaults();
        testInterruptWriteFails();
        testAssignAboveFdSetsize();
        testOpenAboveFdSetsize();
        testAcceptAboveFdSetsize();
    }
};

TEST_SUITE(select_faults, "boost.corosio.fault.select");

} // boost::corosio::test::fault

#endif
