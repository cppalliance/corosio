//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/udp.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/timer.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstring>
#include <stop_token>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {
namespace {

template<auto Backend>
struct udp_socket_test
{
    void testConstruction()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpenV6()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        sock.open(udp::v6());
        BOOST_TEST_EQ(sock.is_open(), true);

        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        udp_socket sock1(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);

        udp_socket sock2(std::move(sock1));
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        udp_socket sock1(ioc);
        udp_socket sock2(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);
        BOOST_TEST_EQ(sock2.is_open(), false);

        sock2 = std::move(sock1);
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    void testBind()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        // Port should have been assigned
        auto ep = sock.local_endpoint();
        BOOST_TEST_GT(ep.port(), 0);

        sock.close();
    }

    void testBindV6()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v6());

        auto ec = sock.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        auto ep = sock.local_endpoint();
        BOOST_TEST_GT(ep.port(), 0);

        sock.close();
    }

    void testSetOption()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::receive_buffer_size(65536));
        auto opt = sock.get_option<socket_option::receive_buffer_size>();
        // Kernel may double the value
        BOOST_TEST_GE(opt.value(), 65536);

        sock.set_option(socket_option::broadcast(true));
        auto bc = sock.get_option<socket_option::broadcast>();
        BOOST_TEST(bc.value());

        sock.close();
    }

    void testSendRecvLoopback()
    {
        io_context ioc(Backend);

        udp_socket sender(ioc);
        udp_socket receiver(ioc);

        sender.open();
        receiver.open();

        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        auto task = [](udp_socket& s, udp_socket& r,
                       endpoint dest) -> capy::task<> {
            // Send a datagram
            char const msg[] = "hello udp";
            auto [ec1, n1] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(ec1, std::error_code{});
            BOOST_TEST_EQ(n1, sizeof(msg));

            // Receive the datagram
            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "hello udp"), 0);

            // Source should be the sender (loopback, ephemeral port)
            BOOST_TEST_EQ(source.v4_address(), ipv4_address::loopback());
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, recv_ep));
        ioc.run();
    }

    void testSendRecvV6Loopback()
    {
        io_context ioc(Backend);

        udp_socket sender(ioc);
        udp_socket receiver(ioc);

        sender.open(udp::v6());
        receiver.open(udp::v6());

        auto ec = receiver.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        auto task = [](udp_socket& s, udp_socket& r,
                       endpoint dest) -> capy::task<> {
            char const msg[] = "hello v6";
            auto [ec1, n1] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(ec1, std::error_code{});
            BOOST_TEST_EQ(n1, sizeof(msg));

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "hello v6"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, recv_ep));
        ioc.run();
    }

    void testEchoLoopback()
    {
        io_context ioc(Backend);

        udp_socket a(ioc);
        udp_socket b(ioc);

        a.open();
        b.open();

        auto ec1 = a.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec1, std::error_code{});
        auto ec2 = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec2, std::error_code{});
        auto b_ep = b.local_endpoint();

        // Single task: send from a to b, then recv on b
        auto task = [](udp_socket& s, udp_socket& r,
                       endpoint dest) -> capy::task<> {
            char const msg[] = "roundtrip";
            auto [ec, n] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(ec, std::error_code{});

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "roundtrip"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(a, b, b_ep));
        ioc.run();
    }

    void testMultipleDatagrams()
    {
        io_context ioc(Backend);

        udp_socket sender(ioc);
        udp_socket receiver(ioc);

        sender.open();
        receiver.open();

        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        auto task = [](udp_socket& s, udp_socket& r,
                       endpoint dest) -> capy::task<> {
            // Send three datagrams
            for (int i = 0; i < 3; ++i)
            {
                char msg[2] = {static_cast<char>('A' + i), '\0'};
                auto [ec, n] =
                    co_await s.send_to(capy::const_buffer(msg, 2), dest);
                BOOST_TEST_EQ(ec, std::error_code{});
                BOOST_TEST_EQ(n, 2u);
            }

            // Receive three datagrams
            for (int i = 0; i < 3; ++i)
            {
                char buf[64] = {};
                endpoint source;
                auto [ec, n] = co_await r.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                BOOST_TEST_EQ(ec, std::error_code{});
                BOOST_TEST_EQ(n, 2u);
            }
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, recv_ep));
        ioc.run();
    }

    void testCancelRecv()
    {
        io_context ioc(Backend);

        udp_socket sock(ioc);
        sock.open();
        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        auto task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

            bool recv_done = false;
            std::error_code recv_ec;

            auto nested = [&sock, &recv_done, &recv_ec]() -> capy::task<> {
                char buf[64];
                endpoint source;
                auto [ec, n] = co_await sock.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                recv_ec   = ec;
                recv_done = true;
            };
            capy::run_async(ioc.get_executor())(nested());

            (void)co_await t.wait();
            sock.cancel();

            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(recv_done);
            BOOST_TEST(recv_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        sock.close();
    }

    void testCloseWhileRecving()
    {
        io_context ioc(Backend);

        udp_socket sock(ioc);
        sock.open();
        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        auto task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

            bool recv_done = false;
            std::error_code recv_ec;

            auto nested = [&sock, &recv_done, &recv_ec]() -> capy::task<> {
                char buf[64];
                endpoint source;
                auto [ec, n] = co_await sock.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                recv_ec   = ec;
                recv_done = true;
            };
            capy::run_async(ioc.get_executor())(nested());

            (void)co_await t.wait();
            sock.close();

            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(recv_done);
            BOOST_TEST(recv_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
    }

    void testStopTokenCancellation()
    {
        io_context ioc(Backend);

        // Two sockets for synchronization: the reader signals
        // readiness by sending a datagram to the canceller.
        udp_socket reader(ioc);
        udp_socket signal_sock(ioc);

        reader.open();
        signal_sock.open();

        auto ec1 = reader.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec1, std::error_code{});
        auto ec2 = signal_sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec2, std::error_code{});

        auto signal_ep = signal_sock.local_endpoint();

        std::stop_source stop_src;
        bool recv_done    = false;
        bool failsafe_hit = false;
        std::error_code recv_ec;

        // Reader task: signal ready, then block on recv_from
        auto reader_task = [&]() -> capy::task<> {
            char const msg[] = "R";
            (void)co_await reader.send_to(
                capy::const_buffer(msg, 1), signal_ep);

            char buf[64];
            endpoint source;
            auto [ec, n] = co_await reader.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            recv_ec   = ec;
            recv_done = true;
        };

        // Canceller: wait for the ready signal, then request stop
        auto canceller_task = [&]() -> capy::task<> {
            char buf[1];
            endpoint source;
            (void)co_await signal_sock.recv_from(
                capy::mutable_buffer(buf, 1), source);

            stop_src.request_stop();
        };

        auto failsafe_task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(1000));
            auto [ec] = co_await t.wait();
            if (!ec && !recv_done)
            {
                failsafe_hit = true;
                reader.cancel();
            }
        };

        capy::run_async(
            ioc.get_executor(), stop_src.get_token())(reader_task());
        capy::run_async(ioc.get_executor())(canceller_task());
        capy::run_async(ioc.get_executor())(failsafe_task());

        ioc.run();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
        BOOST_TEST(!failsafe_hit);

        reader.close();
        signal_sock.close();
    }

    void testConcurrentSendRecv()
    {
        io_context ioc(Backend);

        udp_socket a(ioc);
        udp_socket b(ioc);

        a.open();
        b.open();

        auto ec1 = a.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec1, std::error_code{});
        auto ec2 = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec2, std::error_code{});

        auto a_ep = a.local_endpoint();
        auto b_ep = b.local_endpoint();

        // Exercise simultaneous send_to + recv_from on the same socket
        auto task = [&]() -> capy::task<> {
            bool recv_done = false;
            std::error_code recv_ec;
            std::size_t recv_n = 0;

            // Launch recv_from on socket a (blocks until data arrives)
            auto recv_coro = [&]() -> capy::task<> {
                char buf[64];
                endpoint source;
                auto [ec, n] = co_await a.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                recv_ec   = ec;
                recv_n    = n;
                recv_done = true;
            };
            capy::run_async(ioc.get_executor())(recv_coro());

            // While a is waiting on recv_from, send from a to b
            char const msg1[] = "from-a";
            auto [ec1, n1]    = co_await a.send_to(
                capy::const_buffer(msg1, sizeof(msg1)), b_ep);
            BOOST_TEST_EQ(ec1, std::error_code{});
            BOOST_TEST_EQ(n1, sizeof(msg1));

            // Receive what a sent on b
            char buf2[64] = {};
            endpoint src2;
            auto [ec2, n2] = co_await b.recv_from(
                capy::mutable_buffer(buf2, sizeof(buf2)), src2);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(std::strcmp(buf2, "from-a"), 0);

            // Now send from b to a to unblock a's recv_from
            char const msg3[] = "to-a";
            auto [ec3, n3]    = co_await b.send_to(
                capy::const_buffer(msg3, sizeof(msg3)), a_ep);
            BOOST_TEST_EQ(ec3, std::error_code{});

            // Wait for recv to complete
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();

            BOOST_TEST(recv_done);
            BOOST_TEST_EQ(recv_ec, std::error_code{});
            BOOST_TEST_EQ(recv_n, sizeof(msg3));
        };

        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        a.close();
        b.close();
    }

    void testEmptyBufferRecv()
    {
        io_context ioc(Backend);

        udp_socket sock(ioc);
        sock.open();
        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        auto task = [&]() -> capy::task<> {
            endpoint source;
            auto [ec, n] = co_await sock.recv_from(
                capy::mutable_buffer(nullptr, 0), source);
            // Zero-length recv completes immediately
            BOOST_TEST_EQ(n, 0u);
        };

        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        sock.close();
    }

    void testEmptyBufferSend()
    {
        io_context ioc(Backend);

        udp_socket sock(ioc);
        sock.open();
        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});

        auto task = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock.send_to(
                capy::const_buffer(nullptr, 0),
                endpoint(ipv4_address::loopback(), 9));
            BOOST_TEST_EQ(n, 0u);
        };

        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        sock.close();
    }

    void testMulticastLoopHops()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::multicast_loop_v4(true));
        auto loop = sock.get_option<socket_option::multicast_loop_v4>();
        BOOST_TEST(loop.value());

        sock.set_option(socket_option::multicast_loop_v4(false));
        loop = sock.get_option<socket_option::multicast_loop_v4>();
        BOOST_TEST(!loop.value());

        sock.set_option(socket_option::multicast_hops_v4(4));
        auto hops = sock.get_option<socket_option::multicast_hops_v4>();
        BOOST_TEST_EQ(hops.value(), 4);

        sock.close();
    }

    void testMulticastLoopHopsV6()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v6());

        sock.set_option(socket_option::multicast_loop_v6(true));
        auto loop = sock.get_option<socket_option::multicast_loop_v6>();
        BOOST_TEST(loop.value());

        sock.set_option(socket_option::multicast_loop_v6(false));
        loop = sock.get_option<socket_option::multicast_loop_v6>();
        BOOST_TEST(!loop.value());

        sock.set_option(socket_option::multicast_hops_v6(4));
        auto hops = sock.get_option<socket_option::multicast_hops_v6>();
        BOOST_TEST_EQ(hops.value(), 4);

        sock.close();
    }

    void testMulticastJoinV4()
    {
        io_context ioc(Backend);

        udp_socket receiver(ioc);
        udp_socket sender(ioc);

        receiver.open();
        sender.open();

        auto ec = receiver.bind(endpoint(ipv4_address::any(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        // Join may fail in CI environments without multicast routing
        try
        {
            receiver.set_option(
                socket_option::join_group_v4(ipv4_address("239.255.0.1")));
        }
        catch (std::system_error const&)
        {
            receiver.close();
            sender.close();
            return;
        }

        receiver.set_option(socket_option::multicast_loop_v4(true));

        auto task = [](udp_socket& s, udp_socket& r,
                       unsigned short port) -> capy::task<> {
            endpoint dest(ipv4_address("239.255.0.1"), port);
            char const msg[] = "mcast";
            auto [ec1, n1] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(ec1, std::error_code{});

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "mcast"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, recv_ep.port()));
        ioc.run();

        // Clean up
        try
        {
            receiver.set_option(
                socket_option::leave_group_v4(ipv4_address("239.255.0.1")));
        }
        catch (...)
        {
        }

        receiver.close();
        sender.close();
    }

    void run()
    {
        testConstruction();
        testOpen();
        testOpenV6();
        testMoveConstruct();
        testMoveAssign();
        testBind();
        testBindV6();
        testSetOption();
        testSendRecvLoopback();
        testSendRecvV6Loopback();
        testEchoLoopback();
        testMultipleDatagrams();
        testCancelRecv();
        testCloseWhileRecving();
        testStopTokenCancellation();
        testConcurrentSendRecv();
        testEmptyBufferRecv();
        testEmptyBufferSend();
        testMulticastLoopHops();
        testMulticastLoopHopsV6();
        testMulticastJoinV4();
    }
};

COROSIO_BACKEND_TESTS(udp_socket_test, "boost.corosio.udp_socket")

} // namespace
} // namespace boost::corosio
