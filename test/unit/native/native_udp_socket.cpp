//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_udp_socket.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_udp.hpp>
#include <boost/corosio/timer.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_udp_socket_test
{
    void testConstruct()
    {
        io_context ctx(Backend);
        native_udp_socket<Backend> s(ctx);
        BOOST_TEST_PASS();
    }

    void testMoveConstruct()
    {
        io_context ctx(Backend);
        native_udp_socket<Backend> s1(ctx);
        s1.open();
        BOOST_TEST(s1.is_open());

        native_udp_socket<Backend> s2(std::move(s1));
        BOOST_TEST(s2.is_open());
    }

    void testPolymorphicSlice()
    {
        io_context ctx(Backend);
        native_udp_socket<Backend> ns(ctx);
        ns.open();

        udp_socket& base = ns;
        BOOST_TEST(base.is_open());

        BOOST_TEST_PASS();
    }

    void testSendRecvLoopback()
    {
        io_context ioc(Backend);

        native_udp_socket<Backend> sender(ioc);
        native_udp_socket<Backend> receiver(ioc);

        sender.open();
        receiver.open();

        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        auto task = [](native_udp_socket<Backend>& s,
                       native_udp_socket<Backend>& r,
                       endpoint dest) -> capy::task<> {
            char const msg[] = "native udp";
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
            BOOST_TEST_EQ(std::strcmp(buf, "native udp"), 0);

            BOOST_TEST_EQ(source.v4_address(), ipv4_address::loopback());
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, recv_ep));
        ioc.run();
    }

    void testCancelRecv()
    {
        io_context ioc(Backend);

        native_udp_socket<Backend> sock(ioc);
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

        native_udp_socket<Backend> sock(ioc);
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

    void testVirtualDispatchFallback()
    {
        // Verify that calling through udp_socket& uses virtual dispatch
        io_context ioc(Backend);

        native_udp_socket<Backend> sender(ioc);
        native_udp_socket<Backend> receiver(ioc);

        sender.open();
        receiver.open();

        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        auto task = [](udp_socket& s, udp_socket& r,
                       endpoint dest) -> capy::task<> {
            char const msg[] = "virtual";
            auto [ec1, n1] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(ec1, std::error_code{});

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(std::strcmp(buf, "virtual"), 0);
        };

        udp_socket& s_ref = sender;
        udp_socket& r_ref = receiver;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(s_ref, r_ref, recv_ep));
        ioc.run();
    }

    void run()
    {
        testConstruct();
        testMoveConstruct();
        testPolymorphicSlice();
        testSendRecvLoopback();
        testCancelRecv();
        testCloseWhileRecving();
        testVirtualDispatchFallback();
    }
};

#if BOOST_COROSIO_HAS_EPOLL
struct native_udp_socket_test_epoll : native_udp_socket_test<epoll>
{};
TEST_SUITE(
    native_udp_socket_test_epoll, "boost.corosio.native.udp_socket.epoll");
#endif

#if BOOST_COROSIO_HAS_SELECT
struct native_udp_socket_test_select : native_udp_socket_test<select>
{};
TEST_SUITE(
    native_udp_socket_test_select, "boost.corosio.native.udp_socket.select");
#endif

#if BOOST_COROSIO_HAS_KQUEUE
struct native_udp_socket_test_kqueue : native_udp_socket_test<kqueue>
{};
TEST_SUITE(
    native_udp_socket_test_kqueue, "boost.corosio.native.udp_socket.kqueue");
#endif

} // namespace boost::corosio
