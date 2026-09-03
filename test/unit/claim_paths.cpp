//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// An op parked in a reactor descriptor slot must be claimed and
// reposted when cancel, close, or release arrives — otherwise it
// would dangle in the slot while its coroutine is torn down. These
// tests park ops on descriptors that will never become ready, then
// drive each claiming entry point from a posted coroutine, so the
// interleaving is deterministic on a single thread.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || \
    BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <stop_token>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// Fill the kernel send buffer through the native handle so the next
// write_some parks instead of completing speculatively.
void
fill_send_buffer(native_handle_type fd)
{
    char junk[4096] = {};
    while (::send(fd, junk, sizeof(junk), MSG_DONTWAIT | MSG_NOSIGNAL) > 0)
    {
    }
}

} // namespace

template<auto Backend>
struct claim_paths_test
{
    // Parks reader+writer+waiter on s1, then runs `disrupt` from a
    // posted coroutine and expects all three to complete canceled.
    template<class Disrupt>
    void
    checkStreamClaims(Disrupt disrupt)
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        BOOST_TEST_NO_THROW(
            s1.set_option(socket_option::send_buffer_size(4096)));
        fill_send_buffer(s1.native_handle());

        char buf[8];
        char big[65536] = {};
        std::error_code rec, wec, wtec;
        int done    = 0;
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            ++done;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.write_some(capy::const_buffer(big, sizeof(big)));
            std::ignore = n;
            wec         = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wtec      = ec;
            ++done;
        };
        auto disruptor = [&]() -> capy::task<> {
            disrupt(s1);
            co_return;
        };
        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(disruptor());
        ioc.run();

        BOOST_TEST_EQ(done, 3);
        BOOST_TEST(rec == capy::cond::canceled);
        BOOST_TEST(wec == capy::cond::canceled);
        BOOST_TEST(wtec == capy::cond::canceled);
    }

    void
    testCancelClaimsParkedOps()
    {
        checkStreamClaims([](tcp_socket& s) { s.cancel(); });
    }

    void
    testCloseClaimsParkedOps()
    {
        checkStreamClaims([](tcp_socket& s) { s.close(); });
    }

    void
    testReleaseClaimsParkedOps()
    {
        checkStreamClaims([](tcp_socket& s) {
            auto fd = s.release();
            if (fd >= 0)
                ::close(fd);
        });
    }

    void
    testStopTokenClaimsParkedRead()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        char buf[8];
        std::error_code rec;
        bool done   = false;
        auto reader = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = n;
            rec         = ec;
            done        = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(rec == capy::cond::canceled);
    }

    template<class Disrupt>
    void
    checkAcceptorClaims(Disrupt disrupt)
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST_NO_THROW(acc.set_option(socket_option::reuse_address(true)));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        tcp_socket peer(ioc);
        std::error_code aec, wec;
        int done      = 0;
        auto accepter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wec       = ec;
            ++done;
        };
        auto disruptor = [&]() -> capy::task<> {
            disrupt(acc);
            co_return;
        };
        capy::run_async(ex)(accepter());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(disruptor());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(aec == capy::cond::canceled);
        BOOST_TEST(wec == capy::cond::canceled);
        BOOST_TEST(!peer.is_open());
    }

    void
    testAcceptorCancelClaims()
    {
        checkAcceptorClaims([](tcp_acceptor& a) { a.cancel(); });
    }

    void
    testAcceptorCloseClaims()
    {
        checkAcceptorClaims([](tcp_acceptor& a) { a.close(); });
    }

    void
    testAcceptorReleaseClaims()
    {
        checkAcceptorClaims([](tcp_acceptor& a) {
            auto fd = a.release();
            if (fd >= 0)
                ::close(fd);
        });
    }

    void
    testAcceptorStopTokenClaims()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST_NO_THROW(acc.set_option(socket_option::reuse_address(true)));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        tcp_socket peer(ioc);
        std::error_code aec;
        bool done     = false;
        auto accepter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            done      = true;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ex, ss.get_token())(accepter());
        capy::run_async(ex)(stopper());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(aec == capy::cond::canceled);
    }

    void
    testAcceptorCloseWhileEnqueued()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Both acceptors become readable before the reactor sweeps, so
        // both descriptors are enqueued together; whichever accept
        // handler dispatches first closes an acceptor that is still
        // sitting in the ready queue.
        tcp_acceptor a(ioc), b(ioc);
        for (tcp_acceptor* acc : {&a, &b})
        {
            BOOST_TEST(!acc->open(tcp::v4()));
            BOOST_TEST_NO_THROW(
                acc->set_option(socket_option::reuse_address(true)));
            BOOST_TEST(!acc->bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc->listen());
        }

        tcp_socket pa(ioc), pb(ioc);
        std::error_code aec, bec;
        int done      = 0;
        auto accept_a = [&]() -> capy::task<> {
            auto [ec] = co_await a.accept(pa);
            aec       = ec;
            ++done;
            if (!ec)
                b.close();
        };
        auto accept_b = [&]() -> capy::task<> {
            auto [ec] = co_await b.accept(pb);
            bec       = ec;
            ++done;
            if (!ec)
                a.close();
        };
        auto connect_both = [&]() -> capy::task<> {
            for (tcp_acceptor* acc : {&a, &b})
            {
                int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                BOOST_TEST_GE(fd, 0);
                sockaddr_in sa{};
                sa.sin_family      = AF_INET;
                sa.sin_port        = htons(acc->local_endpoint().port());
                sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                BOOST_TEST_EQ(::connect(
                    fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
                ::close(fd);
            }
            co_return;
        };
        capy::run_async(ex)(accept_a());
        capy::run_async(ex)(accept_b());
        capy::run_async(ex)(connect_both());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        // One accept wins and closes the other; which one is dispatch
        // order, so only the outcome set is asserted.
        BOOST_TEST((!aec && bec == capy::cond::canceled) ||
                   (!bec && aec == capy::cond::canceled) || (!aec && !bec));
    }

    void
    testAcceptorReleaseWhileEnqueued()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // The udp descriptor registers first, so when one sweep makes
        // both descriptors ready, the udp read dispatches first and
        // resumes inline; releasing the acceptor there catches its
        // descriptor state still enqueued behind the udp one.
        udp_socket trigger(ioc);
        BOOST_TEST(!trigger.open(udp::v4()));
        BOOST_TEST(!trigger.bind(endpoint(ipv4_address::loopback(), 0)));

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open(tcp::v4()));
        BOOST_TEST_NO_THROW(acc.set_option(socket_option::reuse_address(true)));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        tcp_socket peer(ioc);
        char buf[8];
        endpoint src;
        std::error_code rec, aec;
        int done      = 0;
        auto releaser = [&]() -> capy::task<> {
            auto [ec, n] = co_await trigger.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), src);
            std::ignore = n;
            rec         = ec;
            auto fd     = acc.release();
            if (fd >= 0)
                ::close(fd);
            ++done;
        };
        auto accepter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            aec       = ec;
            ++done;
        };
        auto trip = [&]() -> capy::task<> {
            sockaddr_in sa{};
            sa.sin_family      = AF_INET;
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            int ufd = ::socket(AF_INET, SOCK_DGRAM, 0);
            BOOST_TEST_GE(ufd, 0);
            sa.sin_port = htons(trigger.local_endpoint().port());
            BOOST_TEST_EQ(::sendto(ufd, "x", 1, 0,
                reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 1);
            ::close(ufd);

            int tfd = ::socket(AF_INET, SOCK_STREAM, 0);
            BOOST_TEST_GE(tfd, 0);
            sa.sin_port = htons(acc.local_endpoint().port());
            BOOST_TEST_EQ(::connect(
                tfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
            ::close(tfd);
            co_return;
        };
        capy::run_async(ex)(releaser());
        capy::run_async(ex)(accepter());
        capy::run_async(ex)(trip());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(!rec);
        // Dispatch order decides whether the accept was still parked
        // when the release landed.
        BOOST_TEST(aec == capy::cond::canceled || !aec);
    }

    void
    testDatagramCloseClaims()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc);
        BOOST_TEST(!s1.open(udp::v4()));
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));

        char buf[8];
        endpoint source;
        std::error_code rec, wtec;
        int done      = 0;
        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            std::ignore = n;
            rec         = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wtec      = ec;
            ++done;
        };
        auto closer = [&]() -> capy::task<> {
            s1.close();
            co_return;
        };
        capy::run_async(ex)(receiver());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(rec == capy::cond::canceled);
        BOOST_TEST(wtec == capy::cond::canceled);
    }

    void
    run()
    {
        testCancelClaimsParkedOps();
        testCloseClaimsParkedOps();
        testReleaseClaimsParkedOps();
        testStopTokenClaimsParkedRead();
        testAcceptorCancelClaims();
        testAcceptorCloseClaims();
        testAcceptorReleaseClaims();
        testAcceptorStopTokenClaims();
        testAcceptorCloseWhileEnqueued();
        testAcceptorReleaseWhileEnqueued();
        testDatagramCloseClaims();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(claim_paths_test, "boost.corosio.claim_paths")

} // namespace boost::corosio

#endif
