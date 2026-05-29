//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/native/native_local_datagram_socket.hpp>

#include <boost/corosio/detail/platform.hpp>

// AF_UNIX SOCK_DGRAM is POSIX-only in practice. Windows added AF_UNIX
// in Win10 1803 but never SOCK_DGRAM over it, so WSASocket fails on
// the very first open() and every test in this file would throw.
#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct native_local_datagram_socket_test
{
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .send_to(
                             std::declval<capy::const_buffer>(),
                             std::declval<local_endpoint>())),
            decltype(std::declval<local_datagram_socket&>().send_to(
                std::declval<capy::const_buffer>(),
                std::declval<local_endpoint>()))>,
        "native_local_datagram_socket::send_to must shadow local_datagram_socket::send_to");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .recv_from(
                             std::declval<capy::mutable_buffer>(),
                             std::declval<local_endpoint&>())),
            decltype(std::declval<local_datagram_socket&>().recv_from(
                std::declval<capy::mutable_buffer>(),
                std::declval<local_endpoint&>()))>,
        "native_local_datagram_socket::recv_from must shadow local_datagram_socket::recv_from");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .connect(std::declval<local_endpoint>())),
            decltype(std::declval<local_datagram_socket&>().connect(
                std::declval<local_endpoint>()))>,
        "native_local_datagram_socket::connect must shadow local_datagram_socket::connect");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .send(std::declval<capy::const_buffer>())),
            decltype(std::declval<local_datagram_socket&>().send(
                std::declval<capy::const_buffer>()))>,
        "native_local_datagram_socket::send must shadow local_datagram_socket::send");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .recv(std::declval<capy::mutable_buffer>())),
            decltype(std::declval<local_datagram_socket&>().recv(
                std::declval<capy::mutable_buffer>()))>,
        "native_local_datagram_socket::recv must shadow local_datagram_socket::recv");
    static_assert(
        !std::is_same_v<
            decltype(std::declval<native_local_datagram_socket<Backend>&>()
                         .wait(wait_type::read)),
            decltype(std::declval<local_datagram_socket&>().wait(
                wait_type::read))>,
        "native_local_datagram_socket::wait must shadow local_datagram_socket::wait");

    void testConstruct()
    {
        io_context ioc(Backend);
        native_local_datagram_socket<Backend> s(ioc);
        BOOST_TEST_EQ(s.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        native_local_datagram_socket<Backend> s(ioc);
        s.open();
        BOOST_TEST(s.is_open());
        s.close();
        BOOST_TEST_EQ(s.is_open(), false);
    }

    void testPolymorphicSlice()
    {
        io_context ioc(Backend);
        native_local_datagram_socket<Backend> s(ioc);
        s.open();
        local_datagram_socket& base = s;
        BOOST_TEST(base.is_open());
    }

    void testSendToRecvFrom()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        auto path1 = tmp1.path();
        auto path2 = tmp2.path();

        native_local_datagram_socket<Backend> sender(ioc);
        native_local_datagram_socket<Backend> receiver(ioc);
        sender.open();
        receiver.open();

        auto ec1 = sender.bind(local_endpoint(path1));
        auto ec2 = receiver.bind(local_endpoint(path2));
        BOOST_TEST_EQ(ec1, std::error_code{});
        BOOST_TEST_EQ(ec2, std::error_code{});

        auto task =
            [](native_local_datagram_socket<Backend>& s,
               native_local_datagram_socket<Backend>& r,
               local_endpoint dest) -> capy::task<> {
            char const msg[] = "native dgram";
            auto [sec, sn] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(sec, std::error_code{});
            BOOST_TEST_EQ(sn, sizeof(msg));

            char buf[64] = {};
            local_endpoint source;
            auto [rec, rn] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(rn, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "native dgram"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, receiver, local_endpoint(path2)));
        ioc.run();
    }

    void testSendRecvConnected()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp_a;
        test::temp_socket_dir tmp_b;
        auto path_a = tmp_a.path();
        auto path_b = tmp_b.path();

        native_local_datagram_socket<Backend> a(ioc);
        native_local_datagram_socket<Backend> b(ioc);
        a.open();
        b.open();

        auto eca = a.bind(local_endpoint(path_a));
        auto ecb = b.bind(local_endpoint(path_b));
        BOOST_TEST_EQ(eca, std::error_code{});
        BOOST_TEST_EQ(ecb, std::error_code{});

        auto task =
            [](native_local_datagram_socket<Backend>& a,
               native_local_datagram_socket<Backend>& b,
               local_endpoint a_to_b,
               local_endpoint b_to_a) -> capy::task<> {
            auto [ec1] = co_await a.connect(a_to_b);
            BOOST_TEST_EQ(ec1, std::error_code{});
            auto [ec2] = co_await b.connect(b_to_a);
            BOOST_TEST_EQ(ec2, std::error_code{});

            char const msg[] = "connected dgram";
            auto [sec, sn] =
                co_await a.send(capy::const_buffer(msg, sizeof(msg)));
            BOOST_TEST_EQ(sec, std::error_code{});
            BOOST_TEST_EQ(sn, sizeof(msg));

            char buf[64] = {};
            auto [rec, rn] =
                co_await b.recv(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(rn, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "connected dgram"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(
            a, b, local_endpoint(path_b), local_endpoint(path_a)));
        ioc.run();
    }

    void testVirtualDispatchFallback()
    {
        io_context ioc(Backend);
        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        auto path1 = tmp1.path();
        auto path2 = tmp2.path();

        native_local_datagram_socket<Backend> sender(ioc);
        native_local_datagram_socket<Backend> receiver(ioc);
        sender.open();
        receiver.open();

        auto ec1 = sender.bind(local_endpoint(path1));
        auto ec2 = receiver.bind(local_endpoint(path2));
        BOOST_TEST_EQ(ec1, std::error_code{});
        BOOST_TEST_EQ(ec2, std::error_code{});

        local_datagram_socket& s_ref = sender;
        local_datagram_socket& r_ref = receiver;

        auto task = [](local_datagram_socket& s, local_datagram_socket& r,
                       local_endpoint dest) -> capy::task<> {
            char const msg[] = "virtual";
            auto [sec, sn] =
                co_await s.send_to(capy::const_buffer(msg, sizeof(msg)), dest);
            BOOST_TEST_EQ(sec, std::error_code{});

            char buf[64] = {};
            local_endpoint source;
            auto [rec, rn] = co_await r.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(rec, std::error_code{});
            BOOST_TEST_EQ(std::strcmp(buf, "virtual"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(s_ref, r_ref, local_endpoint(path2)));
        ioc.run();
    }

    // Exercise the shadowed wait() awaitable: wait_type::read on a
    // bound datagram socket resolves when a datagram arrives.
    void testWait()
    {
        io_context ioc(Backend);
        auto       ex      = ioc.get_executor();
        test::temp_socket_dir rx_tmp;
        auto       rx_path = rx_tmp.path();

        native_local_datagram_socket<Backend> recv(ioc);
        recv.open();
        auto bec = recv.bind(local_endpoint(rx_path));
        BOOST_TEST(!bec);

        native_local_datagram_socket<Backend> send(ioc);
        send.open();

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
                local_endpoint(rx_path));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    void testSendOnClosedThrows()
    {
        io_context ioc(Backend);
        native_local_datagram_socket<Backend> s(ioc);
        char const m[] = "x";

        bool caught_send = false;
        try
        {
            (void)s.send(capy::const_buffer(m, 1));
        }
        catch (std::logic_error const&)
        {
            caught_send = true;
        }
        BOOST_TEST(caught_send);

        bool caught_send_to = false;
        try
        {
            (void)s.send_to(
                capy::const_buffer(m, 1), local_endpoint("/tmp/x"));
        }
        catch (std::logic_error const&)
        {
            caught_send_to = true;
        }
        BOOST_TEST(caught_send_to);

        char buf[1];
        bool caught_recv = false;
        try
        {
            (void)s.recv(capy::mutable_buffer(buf, 1));
        }
        catch (std::logic_error const&)
        {
            caught_recv = true;
        }
        BOOST_TEST(caught_recv);

        local_endpoint src;
        bool caught_recv_from = false;
        try
        {
            (void)s.recv_from(capy::mutable_buffer(buf, 1), src);
        }
        catch (std::logic_error const&)
        {
            caught_recv_from = true;
        }
        BOOST_TEST(caught_recv_from);
    }

    void testConnectAutoOpens()
    {
        // connect() on a closed socket should auto-open then attempt to
        // connect. We aim it at a path that does not exist so the
        // connect itself yields an error, but the auto-open branch
        // is exercised.
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        // temp dir exists, but the socket file inside it does not
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        native_local_datagram_socket<Backend> s(ioc);
        BOOST_TEST_EQ(s.is_open(), false);

        std::error_code result_ec;
        bool done = false;

        capy::run_async(ex)(
            [](native_local_datagram_socket<Backend>& sock,
               local_endpoint ep,
               std::error_code& ec_out, bool& d) -> capy::task<> {
                auto [ec] = co_await sock.connect(ep);
                ec_out = ec;
                d      = true;
            }(s, local_endpoint(path), result_ec, done));

        ioc.run();
        BOOST_TEST(done);
        // socket was opened by the connect() call
        BOOST_TEST(s.is_open());
    }

    void run()
    {
        testConstruct();
        testOpen();
        testPolymorphicSlice();
        testSendToRecvFrom();
        testSendRecvConnected();
        testVirtualDispatchFallback();
        testWait();
        testSendOnClosedThrows();
        testConnectAutoOpens();
    }
};

COROSIO_BACKEND_TESTS(
    native_local_datagram_socket_test,
    "boost.corosio.native.local_datagram_socket")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
