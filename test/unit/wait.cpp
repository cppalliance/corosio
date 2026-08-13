//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header is self-contained.
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <chrono>
#include <string_view>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct wait_test
{
    // wait_read completes when the peer sends data, no bytes consumed.
    void testWaitReadAndNoConsume()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        constexpr std::string_view payload = "hello";

        std::error_code wait_ec;
        bool wait_done = false;
        std::error_code read_ec;
        std::size_t bytes_read = 0;
        std::array<char, 32> buf{};

        auto reader = [&]() -> capy::task<> {
            auto [ec1] = co_await s1.wait(wait_type::read);
            wait_ec    = ec1;
            wait_done  = true;
            if (ec1)
                co_return;
            auto [ec2, n] = co_await s1.read_some(
                capy::mutable_buffer(buf.data(), buf.size()));
            read_ec    = ec2;
            bytes_read = n;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, payload.size());
    }

    // wait_write completes immediately on a freshly connected socket.
    void testWaitWriteImmediate()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto writer = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // UDP wait_read fires when a datagram arrives.
    void testWaitOnUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv(ioc);
        recv.open(udp::v4());
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        udp_socket send(ioc);
        send.open(udp::v4());

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await recv.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char dg[1] = { 'X' };
            auto [ec, n] = co_await send.send_to(
                capy::const_buffer(dg, sizeof(dg)),
                endpoint(ipv4_address::loopback(), port));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Acceptor wait_read fires when a client connects; accept then succeeds.
    void testAcceptorWait()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);
        auto port = acc.local_endpoint().port();

        std::error_code wait_ec;
        bool wait_done = false;
        std::error_code accept_ec;
        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        auto waiter = [&]() -> capy::task<> {
            auto [ec1] = co_await acc.wait(wait_type::read);
            wait_ec    = ec1;
            wait_done  = true;
            if (ec1)
                co_return;
            auto [ec2] = co_await acc.accept(peer);
            accept_ec  = ec2;
        };
        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            (void)ec;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(peer.is_open());
    }

    // local_stream_socket wait_read fires when the peer writes.
    void testWaitOnLocalStream()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto bec = acc.bind(local_endpoint(path));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        client.open();

        auto accept_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(server);
            (void)ec;
        };
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(local_endpoint(path));
            (void)ec;
        };
        capy::run_async(ex)(accept_task());
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();

        constexpr std::string_view payload = "hi";
        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await server.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Cancellation via socket.cancel() yields operation_canceled.
    void testCancellation()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await delay(std::chrono::milliseconds(20));
            s1.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Cancel a UDP wait_read while it's parked. On IOCP this exercises
    // the auxiliary WSAPoll reactor's cancel_wait path, where the op
    // has no overlapped I/O pending so CancelIoEx is a no-op and the
    // cancellation must be delivered through the reactor itself.
    void testUdpCancellation()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await delay(std::chrono::milliseconds(20));
            sock.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // A short read that leaves bytes buffered consumes the readiness
    // edge; a subsequent wait_read must still complete because data
    // remains available.
    void testWaitReadAfterShortRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code read_ec;
        std::size_t bytes_read = 0;
        std::error_code wait_ec;
        bool wait_done = false;
        char first = 0;
        std::array<char, 8> rest{};
        std::size_t rest_n = 0;

        auto reader = [&]() -> capy::task<> {
            auto [ec1, n1] =
                co_await s1.read_some(capy::mutable_buffer(&first, 1));
            read_ec    = ec1;
            bytes_read = n1;
            auto [ec2] = co_await s1.wait(wait_type::read);
            wait_ec    = ec2;
            wait_done  = true;
            if (ec2)
                co_return;
            auto [ec3, n3] = co_await s1.read_some(
                capy::mutable_buffer(rest.data(), rest.size()));
            (void)ec3;
            rest_n = n3;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.write_some(capy::const_buffer("xy", 2));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, 1u);
        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST_EQ(first, 'x');
        BOOST_TEST_EQ(rest_n, 1u);
        BOOST_TEST_EQ(rest[0], 'y');
    }

    // Draining the socket with a speculative read leaves a cached
    // readiness edge behind; a subsequent wait_read must park on the
    // now-empty socket instead of completing on the stale edge. A
    // second socket pair sequences the cancel strictly after the wait
    // is parked.
    void testWaitReadParksAfterDrain()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);
        auto [t1, t2] = test::make_socket_pair(ioc);

        std::size_t drained_n = 0;
        std::error_code wait_ec;
        bool wait_done   = false;
        bool cancel_sent = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto driver = [&]() -> capy::task<> {
            // Latch a readiness edge on s1 with no read op parked...
            auto [wec, wn] = co_await s2.write_some(
                capy::const_buffer("xy", 2));
            (void)wec;
            (void)wn;
            // ...drain it, typically on the speculative success path...
            std::array<char, 8> buf{};
            auto [rec, rn] = co_await s1.read_some(
                capy::mutable_buffer(buf.data(), buf.size()));
            (void)rec;
            drained_n = rn;
            // ...then park the wait before the release signal exists.
            // Spawning here queues the wait initiation ahead of every
            // completion the "go" write can generate, so the cancel is
            // ordered after the park on FIFO schedulers and completion
            // ports alike.
            capy::run_async(ex)(waiter());
            auto [sec, sn] = co_await t2.write_some(
                capy::const_buffer("go", 2));
            (void)sec;
            (void)sn;
        };
        auto canceller = [&]() -> capy::task<> {
            char c[2];
            auto [ec, n] = co_await t1.read_some(
                capy::mutable_buffer(c, sizeof(c)));
            (void)ec;
            (void)n;
            cancel_sent = true;
            s1.cancel();
        };

        capy::run_async(ex)(canceller());
        capy::run_async(ex)(driver());
        ioc.run();

        BOOST_TEST_EQ(drained_n, 2u);
        BOOST_TEST(cancel_sent);
        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // A parked recv consumes the readiness edge but only the first of
    // two buffered datagrams; a subsequent wait_read must complete
    // while the second datagram remains.
    void testWaitReadSecondDatagram()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv(ioc);
        recv.open(udp::v4());
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        udp_socket send(ioc);
        send.open(udp::v4());

        std::size_t first_n = 0;
        std::error_code wait_ec;
        bool wait_done = false;

        auto receiver = [&]() -> capy::task<> {
            char dg[4];
            endpoint source;
            auto [ec, n] = co_await recv.recv_from(
                capy::mutable_buffer(dg, sizeof(dg)), source);
            (void)ec;
            first_n = n;
            auto [wec] = co_await recv.wait(wait_type::read);
            wait_ec   = wec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char a[1] = { 'a' };
            char b[1] = { 'b' };
            endpoint dst(ipv4_address::loopback(), port);
            auto [e1, n1] = co_await send.send_to(
                capy::const_buffer(a, sizeof(a)), dst);
            (void)e1;
            (void)n1;
            auto [e2, n2] = co_await send.send_to(
                capy::const_buffer(b, sizeof(b)), dst);
            (void)e2;
            (void)n2;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST_EQ(first_n, 1u);
        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Draining the only buffered datagram on the speculative path
    // leaves a stale readiness edge; a subsequent wait_read must park
    // on the empty socket instead of completing on the stale edge.
    void testWaitUdpParksAfterDrain()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket rsock(ioc);
        rsock.open(udp::v4());
        auto bec = rsock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        udp_socket ssock(ioc);
        ssock.open(udp::v4());

        auto [t1, t2] = test::make_socket_pair(ioc);

        std::size_t drained_n = 0;
        std::error_code wait_ec;
        bool wait_done   = false;
        bool cancel_sent = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await rsock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto driver = [&]() -> capy::task<> {
            // Latch a readiness edge on rsock with no recv parked...
            char dg[1] = { 'x' };
            auto [wec, wn] = co_await ssock.send_to(
                capy::const_buffer(dg, sizeof(dg)),
                rsock.local_endpoint());
            (void)wec;
            (void)wn;
            // ...drain it, typically on the speculative success path...
            char buf[4];
            endpoint source;
            auto [rec, rn] = co_await rsock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            (void)rec;
            drained_n = rn;
            // ...then park the wait before the release signal exists.
            // Spawning here queues the wait initiation ahead of every
            // completion the "go" write can generate, so the cancel is
            // ordered after the park on FIFO schedulers and completion
            // ports alike.
            capy::run_async(ex)(waiter());
            auto [sec, sn] = co_await t2.write_some(
                capy::const_buffer("go", 2));
            (void)sec;
            (void)sn;
        };
        auto canceller = [&]() -> capy::task<> {
            char c[2];
            auto [ec, n] = co_await t1.read_some(
                capy::mutable_buffer(c, sizeof(c)));
            (void)ec;
            (void)n;
            cancel_sent = true;
            rsock.cancel();
        };

        capy::run_async(ex)(canceller());
        capy::run_async(ex)(driver());
        ioc.run();

        BOOST_TEST_EQ(drained_n, 1u);
        BOOST_TEST(cancel_sent);
        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void run()
    {
        testWaitReadAndNoConsume();
        testWaitWriteImmediate();
        testAcceptorWait();
        testWaitOnLocalStream();
        testWaitOnUdp();
        testWaitReadAfterShortRead();
        testWaitReadParksAfterDrain();
        testWaitReadSecondDatagram();
        testWaitUdpParksAfterDrain();
        testCancellation();
        testUdpCancellation();
    }
};

COROSIO_BACKEND_TESTS(wait_test, "boost.corosio.wait")

// Reactor-only: the readiness probe is shared by epoll/kqueue/select;
// the proactor backends resolve closed-socket waits through their own
// submission paths.
template<auto Backend>
struct wait_closed_test
{
    // Waiting on a socket that was never opened must fail instead of
    // parking forever on a descriptor the reactor has never seen.
    void testWaitOnUnopenedSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_socket sock(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == std::errc::bad_file_descriptor);
    }

    void run()
    {
        testWaitOnUnopenedSocket();
    }
};

COROSIO_REACTOR_BACKEND_TESTS(wait_closed_test, "boost.corosio.wait_closed")

} // namespace boost::corosio
