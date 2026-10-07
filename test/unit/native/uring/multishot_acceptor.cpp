//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/backend.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <tuple>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The coroutine tasks below take their state through by-value parameters
// (references to test-scope objects), never by lambda capture: a captured
// closure is a temporary destroyed at the end of the run_async statement,
// which the suspended coroutine would then read through a dangling `this`.

namespace boost::corosio {

// Exposes the io_uring scheduler's internal in-flight counter so the
// teardown-accounting test can assert it stays balanced.
struct uring_test_context : native_io_context<uring>
{
    std::int64_t inflight()
    {
        return static_cast<detail::uring_scheduler*>(sched_)->inflight();
    }
};

inline capy::task<>
noop_task()
{
    co_return;
}

// Reap completions nobody awaits, such as a cancelled arming's
// terminal CQE: poll() only pumps the ring while work is outstanding.
inline void
reap(native_io_context<uring>& ioc)
{
    for (int i = 0; i < 8; ++i)
    {
        capy::run_async(ioc.get_executor())(noop_task());
        ioc.restart();
        ioc.poll();
    }
    ioc.restart();
}

// A blocking loopback connect; completes from the listener's backlog.
inline int
raw_connect(std::uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

inline std::uint16_t
peer_port(tcp_socket const& s)
{
    return s.remote_endpoint().port();
}

inline std::uint16_t
local_port(int fd)
{
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
    return ntohs(sa.sin_port);
}

struct multishot_acceptor_test
{
    void testContextConstructs()
    {
        native_io_context<uring> ioc;
        BOOST_TEST(!ioc.stopped());
    }

    // Destroying an acceptor must leave uring_inflight_ balanced: the
    // multishot accept SQE and the cancel SQEs close submits are each
    // counted once and must each be uncounted by their CQEs. The
    // counter gates the do_one ring pump, so a leak would accumulate
    // across every acceptor teardown for the lifetime of the io_context.
    void testCloseReapsArming()
    {
        uring_test_context ctx;

        // No io_uring SQEs are counted before any op is submitted.
        BOOST_TEST_EQ(ctx.inflight(), 0);

        {
            tcp_acceptor acc(ctx);
            BOOST_TEST(!acc.open());
            acc.set_option(socket_option::reuse_address(true));
            BOOST_TEST(!acc.bind(endpoint(0)));
            // listen() calls start_multishot(), submitting a multishot
            // accept SQE (counted once).
            BOOST_TEST(!acc.listen());

            // Flush the SQE to the kernel so it is genuinely in flight.
            ctx.poll();
            BOOST_TEST(ctx.inflight() >= 1);
        }
        // acc destroyed: close cancelled the arming, whose terminal CQE
        // the run loop reaps. poll() only pumps the ring while work is
        // outstanding.
        auto ex = ctx.get_executor();
        for (int i = 0; i < 64 && ctx.inflight() != 0; ++i)
        {
            capy::run_async(ex)(noop_task());
            ctx.restart();
            ctx.poll();
        }

        // Every submitted SQE (the multishot accept plus the teardown
        // cancels) is now accounted for.
        BOOST_TEST_EQ(ctx.inflight(), 0);
    }

    // A connection the multishot SQE delivers with no accept parked is
    // buffered in ready_fds_; a later accept() consumes it on the immediate
    // path instead of parking a waiter. The connection is driven to
    // completion first (its accept CQE buffers server-side), then a
    // separate run() posts the accept that consumes the buffered fd.
    void testAcceptBufferedConnection()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        endpoint local = acc.local_endpoint();

        tcp_socket client(ioc);
        bool connected = false;
        capy::run_async(ex)(
            [](tcp_socket& c, endpoint ep, bool& done) -> capy::task<> {
                auto [ec] = co_await c.connect(ep);
                done      = !ec;
            }(client, local, connected));
        ioc.run(); // connector completes; the server accept CQE buffers
        BOOST_TEST(connected);

        ioc.restart();
        bool accepted = false;
        capy::run_async(ex)([](tcp_acceptor& a, bool& done) -> capy::task<> {
            auto [ec, peer] = co_await a.accept();
            done            = !ec && peer.is_open();
        }(acc, accepted));
        ioc.run(); // accept consumes the buffered fd
        BOOST_TEST(accepted);

        client.close();
        acc.close();
    }

    // An accept parked before any connection arrives is matched by the
    // multishot delivery when the connection lands (the waiter-match path,
    // distinct from consuming a pre-buffered fd).
    void testAcceptParkedThenDelivered()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        endpoint local = acc.local_endpoint();

        bool accepted = false;
        capy::run_async(ex)([](tcp_acceptor& a, bool& done) -> capy::task<> {
            auto [ec, peer] = co_await a.accept();
            done            = !ec && peer.is_open();
        }(acc, accepted));
        // Park the accept without a connection present.
        for (int i = 0; i < 16; ++i)
            ioc.poll();
        BOOST_TEST(!accepted);

        tcp_socket client(ioc);
        capy::run_async(ex)([](tcp_socket& c, endpoint ep) -> capy::task<> {
            std::ignore = co_await c.connect(ep);
        }(client, local));
        for (int i = 0; i < 10000 && !accepted; ++i) // delivery matches waiter
            ioc.poll();
        BOOST_TEST(accepted);

        client.close();
        acc.close();
    }

    // Buffered connections never accepted must be closed when the acceptor
    // is destroyed (the ready_fds_ drain in the impl destructor).
    void testDestroyWithBufferedConnections()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();

        tcp_socket c1(ioc), c2(ioc);
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            acc.set_option(socket_option::reuse_address(true));
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            endpoint local = acc.local_endpoint();

            int connected    = 0;
            auto run_connect = [&](tcp_socket& c) {
                capy::run_async(ex)(
                    [](tcp_socket& s, endpoint ep, int& n) -> capy::task<> {
                        auto [ec] = co_await s.connect(ep);
                        if (!ec)
                            ++n;
                    }(c, local, connected));
            };
            run_connect(c1);
            run_connect(c2);
            ioc.run(); // both connect; both server accept CQEs buffer
            BOOST_TEST_EQ(connected, 2);
            // acc destroyed here holding two buffered fds -> ready_fds_ drain.
        }
        c1.close();
        c2.close();
    }

    // Accept one connection through `acc` and report the peer port.
    static std::uint16_t
    accept_one(native_io_context<uring>& ioc, tcp_acceptor& acc)
    {
        tcp_socket peer(ioc);
        bool accepted = false;
        capy::run_async(ioc.get_executor())(
            [](tcp_acceptor& a, tcp_socket& p, bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(p);
                done      = !ec;
            }(acc, peer, accepted));
        for (int i = 0; i < 10000 && !accepted; ++i)
            ioc.poll();
        ioc.restart();
        BOOST_TEST(accepted);
        return accepted ? peer_port(peer) : 0;
    }

    // A released listener keeps its connections: once the acceptor
    // listens again, it accepts only from its new descriptor, even
    // while the released one still has connections queued.
    void testReleaseThenRelisten()
    {
        native_io_context<uring> ioc;

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        int released = static_cast<int>(acc.release());
        BOOST_TEST_GE(released, 0);

        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        int stale = raw_connect(local_port(released));
        int fresh = raw_connect(acc.local_endpoint().port());
        BOOST_TEST_GE(stale, 0);
        BOOST_TEST_GE(fresh, 0);

        BOOST_TEST_EQ(accept_one(ioc, acc), local_port(fresh));

        // The released descriptor still holds its own connection.
        int got = ::accept4(released, nullptr, nullptr, SOCK_NONBLOCK);
        BOOST_TEST_GE(got, 0);

        ::close(got);
        ::close(stale);
        ::close(fresh);
        ::close(released);
        acc.close();
        reap(ioc);
    }

    // Assigning right after a close, while the cancelled arming's
    // terminal completion is still unreaped, moves accepting to the new
    // descriptor; a re-listen on it does not arm a second time.
    void testAssignReplacesArming()
    {
        native_io_context<uring> ioc;

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        int nfd =
            ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_TEST_EQ(
            ::bind(nfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)), 0);
        BOOST_TEST_EQ(::listen(nfd, 16), 0);
        acc.close();
        BOOST_TEST(!acc.assign(nfd));
        BOOST_TEST(!acc.listen());

        for (int i = 0; i < 3; ++i)
        {
            int c = raw_connect(acc.local_endpoint().port());
            BOOST_TEST_GE(c, 0);
            BOOST_TEST_EQ(accept_one(ioc, acc), local_port(c));
            ::close(c);
        }

        acc.close();
        reap(ioc);
    }

    // Close on one thread races deliveries and terminal completions
    // being dispatched on another.
    void testCloseRacesDispatch()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();
        {
            tcp_acceptor warm(ioc);
        }

        std::stop_source keep;
        capy::run_async(ex, keep.get_token())([]() -> capy::task<> {
            std::ignore = co_await delay(std::chrono::seconds(60));
        }());
        std::thread runner([&] { ioc.run(); });

        for (int i = 0; i < 200; ++i)
        {
            tcp_acceptor acc(ioc);
            BOOST_TEST(!acc.open());
            BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!acc.listen());
            int c = raw_connect(acc.local_endpoint().port());
            if (i % 2)
                acc.close();
            ::close(c);
        }

        keep.request_stop();
        runner.join();
    }

    void run()
    {
        testContextConstructs();
        testCloseReapsArming();
        testAcceptBufferedConnection();
        testAcceptParkedThenDelivered();
        testDestroyWithBufferedConnections();
        testReleaseThenRelisten();
        testAssignReplacesArming();
        testCloseRacesDispatch();
    }
};

TEST_SUITE(
    multishot_acceptor_test, "boost.corosio.native.uring.multishot_acceptor");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
