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
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/native/native_udp.hpp>
#include <boost/corosio/timer.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_udp_socket_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().send_to(
                std::declval<capy::const_buffer>(),
                std::declval<endpoint>())),
            decltype(std::declval<udp_socket&>().send_to(
                std::declval<capy::const_buffer>(),
                std::declval<endpoint>()))>,
        "native_udp_socket::send_to must shadow udp_socket::send_to");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().recv_from(
                std::declval<capy::mutable_buffer>(),
                std::declval<endpoint&>())),
            decltype(std::declval<udp_socket&>().recv_from(
                std::declval<capy::mutable_buffer>(),
                std::declval<endpoint&>()))>,
        "native_udp_socket::recv_from must shadow udp_socket::recv_from");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().connect(
                std::declval<endpoint>())),
            decltype(std::declval<udp_socket&>().connect(
                std::declval<endpoint>()))>,
        "native_udp_socket::connect must shadow udp_socket::connect");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().send(
                std::declval<capy::const_buffer>())),
            decltype(std::declval<udp_socket&>().send(
                std::declval<capy::const_buffer>()))>,
        "native_udp_socket::send must shadow udp_socket::send");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().recv(
                std::declval<capy::mutable_buffer>())),
            decltype(std::declval<udp_socket&>().recv(
                std::declval<capy::mutable_buffer>()))>,
        "native_udp_socket::recv must shadow udp_socket::recv");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_udp_socket<Backend>&>().wait(
                wait_type::read)),
            decltype(std::declval<udp_socket&>().wait(wait_type::read))>,
        "native_udp_socket::wait must shadow udp_socket::wait");

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

    void testSendRecvConnected()
    {
        io_context ioc(Backend);

        native_udp_socket<Backend> a(ioc);
        native_udp_socket<Backend> b(ioc);

        b.open();
        auto ec = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto b_ep = b.local_endpoint();

        auto task = [](native_udp_socket<Backend>& a,
                       native_udp_socket<Backend>& b,
                       endpoint dest) -> capy::task<> {
            auto [ec1] = co_await a.connect(dest);
            BOOST_TEST_EQ(ec1, std::error_code{});
            BOOST_TEST(a.is_open());

            char const msg[] = "native connected";
            auto [ec2, n2] =
                co_await a.send(capy::const_buffer(msg, sizeof(msg)));
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));

            char buf[64] = {};
            endpoint source;
            auto [ec3, n3] = co_await b.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec3, std::error_code{});
            BOOST_TEST_EQ(n3, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "native connected"), 0);

            auto [ec4] = co_await b.connect(source);
            BOOST_TEST_EQ(ec4, std::error_code{});

            char const reply[] = "native reply";
            auto [ec5, n5] =
                co_await b.send(capy::const_buffer(reply, sizeof(reply)));
            BOOST_TEST_EQ(ec5, std::error_code{});

            char buf2[64] = {};
            auto [ec6, n6] =
                co_await a.recv(capy::mutable_buffer(buf2, sizeof(buf2)));
            BOOST_TEST_EQ(ec6, std::error_code{});
            BOOST_TEST_EQ(n6, sizeof(reply));
            BOOST_TEST_EQ(std::strcmp(buf2, "native reply"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(a, b, b_ep));
        ioc.run();
    }

    void testConnectAutoOpen()
    {
        io_context ioc(Backend);

        native_udp_socket<Backend> receiver(ioc);
        receiver.open();
        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        native_udp_socket<Backend> sender(ioc);
        BOOST_TEST_EQ(sender.is_open(), false);

        auto task = [](native_udp_socket<Backend>& s,
                       endpoint dest) -> capy::task<> {
            auto [ec] = co_await s.connect(dest);
            BOOST_TEST_EQ(ec, std::error_code{});
            BOOST_TEST(s.is_open());
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, recv_ep));
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

    // Exercise the shadowed wait() awaitable: wait_type::read on a
    // bound UDP socket resolves when a datagram arrives from a peer.
    void testWait()
    {
        io_context ioc(Backend);
        auto       ex = ioc.get_executor();

        native_udp_socket<Backend> recv(ioc);
        recv.open(udp::v4());
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        native_udp_socket<Backend> send(ioc);
        send.open(udp::v4());

        std::error_code wait_ec;
        bool            wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await recv.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char dg[1]   = {'X'};
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

    // Exercise the inline native_socket_option types on a UDP socket.
    // The public socket_option:: variants are tested elsewhere; this
    // hits the templated boolean<>/integer<> instantiations and the
    // multicast option storage classes without library indirection.
    void testNativeSocketOptions()
    {
        io_context ioc(Backend);
        native_udp_socket<Backend> sock(ioc);
        sock.open();

        sock.set_option(native_socket_option::broadcast(true));
        auto bc =
            sock.template get_option<native_socket_option::broadcast>();
        BOOST_TEST(bc.value());

        sock.set_option(native_socket_option::receive_buffer_size(32768));
        auto rb = sock.template get_option<
            native_socket_option::receive_buffer_size>();
        BOOST_TEST(rb.value() > 0);

        sock.set_option(native_socket_option::send_buffer_size(32768));
        auto sb = sock.template get_option<
            native_socket_option::send_buffer_size>();
        BOOST_TEST(sb.value() > 0);

        sock.set_option(native_socket_option::multicast_loop_v4(true));
        auto ml = sock.template get_option<
            native_socket_option::multicast_loop_v4>();
        BOOST_TEST(ml.value());

        sock.set_option(native_socket_option::multicast_hops_v4(4));
        auto mh = sock.template get_option<
            native_socket_option::multicast_hops_v4>();
        BOOST_TEST_EQ(mh.value(), 4);

        // Multicast configuration is environment-specific; the option
        // type's storage and the library set_option path are exercised
        // regardless of whether the kernel completes the request.
        try
        {
            sock.set_option(native_socket_option::multicast_interface_v4(
                ipv4_address::any()));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        // Default-constructed variants exercise the no-arg constructors.
        native_socket_option::multicast_interface_v4 mif4;
        BOOST_TEST_EQ(mif4.size(), sizeof(struct in_addr));
        BOOST_TEST(mif4.data() != nullptr);
        BOOST_TEST_EQ(mif4.level(), IPPROTO_IP);
        BOOST_TEST_EQ(mif4.name(), IP_MULTICAST_IF);

        sock.close();
    }

    void testNativeMulticastV4Groups()
    {
        io_context ioc(Backend);
        native_udp_socket<Backend> sock(ioc);
        sock.open();

        try
        {
            sock.set_option(native_socket_option::join_group_v4(
                ipv4_address("239.255.0.3")));
            sock.set_option(native_socket_option::leave_group_v4(
                ipv4_address("239.255.0.3")));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        // Default-constructed forms — verifies no-arg ctor coverage.
        native_socket_option::join_group_v4 jg;
        BOOST_TEST_EQ(jg.size(), sizeof(struct ip_mreq));
        BOOST_TEST(jg.data() != nullptr);
        BOOST_TEST_EQ(jg.level(), IPPROTO_IP);
        BOOST_TEST_EQ(jg.name(), IP_ADD_MEMBERSHIP);

        native_socket_option::leave_group_v4 lg;
        BOOST_TEST_EQ(lg.size(), sizeof(struct ip_mreq));
        BOOST_TEST(lg.data() != nullptr);
        BOOST_TEST_EQ(lg.level(), IPPROTO_IP);
        BOOST_TEST_EQ(lg.name(), IP_DROP_MEMBERSHIP);

        sock.close();
    }

    void testNativeMulticastV6Groups()
    {
        io_context ioc(Backend);
        native_udp_socket<Backend> sock(ioc);
        sock.open(udp::v6());

        sock.set_option(native_socket_option::multicast_loop_v6(true));
        sock.set_option(native_socket_option::multicast_hops_v6(4));

        try
        {
            sock.set_option(native_socket_option::multicast_interface_v6(0));
            auto mif6 = sock.template get_option<
                native_socket_option::multicast_interface_v6>();
            BOOST_TEST_EQ(mif6.value(), 0);
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        try
        {
            sock.set_option(native_socket_option::join_group_v6(
                ipv6_address("ff02::1")));
            sock.set_option(native_socket_option::leave_group_v6(
                ipv6_address("ff02::1")));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        // Default-constructed forms — verifies no-arg ctor coverage.
        native_socket_option::join_group_v6 jg;
        BOOST_TEST_EQ(jg.size(), sizeof(struct ipv6_mreq));
        BOOST_TEST(jg.data() != nullptr);
        BOOST_TEST_EQ(jg.level(), IPPROTO_IPV6);
        BOOST_TEST_EQ(jg.name(), IPV6_JOIN_GROUP);

        native_socket_option::leave_group_v6 lg;
        BOOST_TEST_EQ(lg.size(), sizeof(struct ipv6_mreq));
        BOOST_TEST(lg.data() != nullptr);
        BOOST_TEST_EQ(lg.level(), IPPROTO_IPV6);
        BOOST_TEST_EQ(lg.name(), IPV6_LEAVE_GROUP);

        sock.close();
    }

    void testNativeLinger()
    {
        // Exercises native_socket_option::linger (default + member setters).
        native_socket_option::linger lg;
        BOOST_TEST(!lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 0);
        BOOST_TEST_EQ(lg.size(), sizeof(::linger));
        BOOST_TEST(lg.data() != nullptr);

        native_socket_option::linger const& clg = lg;
        BOOST_TEST(clg.data() != nullptr);

        lg.enabled(true);
        lg.timeout(7);
        BOOST_TEST(lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 7);
        BOOST_TEST_EQ(lg.level(), SOL_SOCKET);
        BOOST_TEST_EQ(lg.name(), SO_LINGER);

        native_socket_option::linger lg2(true, 3);
        BOOST_TEST(lg2.enabled());
        BOOST_TEST_EQ(lg2.timeout(), 3);
    }

    void run()
    {
        testConstruct();
        testMoveConstruct();
        testPolymorphicSlice();
        testSendRecvLoopback();
        testSendRecvConnected();
        testConnectAutoOpen();
        testCancelRecv();
        testCloseWhileRecving();
        testVirtualDispatchFallback();
        testWait();
        testNativeSocketOptions();
        testNativeMulticastV4Groups();
        testNativeMulticastV6Groups();
        testNativeLinger();
    }
};

COROSIO_BACKEND_TESTS(native_udp_socket_test, "boost.corosio.native.udp_socket")

} // namespace boost::corosio
