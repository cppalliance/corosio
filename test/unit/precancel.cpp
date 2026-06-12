//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Initiate every async operation with a stop token that is already
// stopped. Each op must complete with `canceled` without performing
// any I/O. This exercises the stop_now short-circuit at the head of
// each initiation path, which no cancellation-after-park test reaches.

#include <boost/corosio/detail/platform.hpp>

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

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <boost/corosio/test/temp_path.hpp>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct precancel_test
{
    void testTcpSocket()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        std::error_code read_ec, write_ec, wait_ec;
        int done = 0;

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            read_ec = ec;
            ++done;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.write_some(capy::const_buffer("x", 1));
            (void)n;
            write_ec = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex, ss.get_token())(waiter());
        ioc.run();

        BOOST_TEST_EQ(done, 3);
        BOOST_TEST(read_ec == capy::cond::canceled);
        BOOST_TEST(write_ec == capy::cond::canceled);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void testTcpConnect()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_socket sock(ioc);
        sock.open();

        std::stop_source ss;
        ss.request_stop();

        std::error_code conn_ec;
        bool done = false;

        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await sock.connect(
                endpoint(ipv4_address::loopback(), 1));
            conn_ec = ec;
            done    = true;
        };

        capy::run_async(ex, ss.get_token())(connector());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(conn_ec == capy::cond::canceled);
    }

    void testTcpAccept()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        ss.request_stop();

        tcp_socket peer(ioc);
        std::error_code accept_ec;
        bool done = false;

        auto acceptor_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(peer);
            accept_ec = ec;
            done      = true;
        };

        capy::run_async(ex, ss.get_token())(acceptor_task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(accept_ec == capy::cond::canceled);
        BOOST_TEST(!peer.is_open());
    }

    void testUdpSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc), s2(ioc);
        s1.open(udp::v4());
        s2.open(udp::v4());
        BOOST_TEST(!s1.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!s2.bind(endpoint(ipv4_address::loopback(), 0)));
        auto peer_ep = s2.local_endpoint();

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        endpoint source;
        std::error_code send_to_ec, recv_from_ec, conn_ec, wait_ec;
        int done = 0;

        auto send_to_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send_to(
                capy::const_buffer("x", 1), peer_ep);
            (void)n;
            send_to_ec = ec;
            ++done;
        };
        auto recv_from_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            (void)n;
            recv_from_ec = ec;
            ++done;
        };
        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(peer_ep);
            conn_ec   = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(send_to_task());
        capy::run_async(ex, ss.get_token())(recv_from_task());
        capy::run_async(ex, ss.get_token())(connector());
        capy::run_async(ex, ss.get_token())(waiter());
        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(done, 4);
        BOOST_TEST(send_to_ec == capy::cond::canceled);
        BOOST_TEST(recv_from_ec == capy::cond::canceled);
        BOOST_TEST(conn_ec == capy::cond::canceled);
        BOOST_TEST(wait_ec == capy::cond::canceled);

        // send()/recv() require a connected socket, so complete a real
        // connect before initiating them with the stopped token.
        bool conn_ok_done            = false;
        std::error_code conn_ok_ec;
        auto real_connector = [&]() -> capy::task<> {
            auto [ec]    = co_await s1.connect(peer_ep);
            conn_ok_ec   = ec;
            conn_ok_done = true;
        };
        capy::run_async(ex)(real_connector());
        ioc.run();
        ioc.restart();
        BOOST_TEST(conn_ok_done);
        BOOST_TEST(!conn_ok_ec);

        std::error_code send_ec, recv_ec;
        auto send_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send(capy::const_buffer("x", 1));
            (void)n;
            send_ec = ec;
            ++done;
        };
        auto recv_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            recv_ec = ec;
            ++done;
        };
        capy::run_async(ex, ss.get_token())(send_task());
        capy::run_async(ex, ss.get_token())(recv_task());
        ioc.run();

        BOOST_TEST_EQ(done, 6);
        BOOST_TEST(send_ec == capy::cond::canceled);
        BOOST_TEST(recv_ec == capy::cond::canceled);
    }

#if BOOST_COROSIO_POSIX

    void testLocalStreamSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        std::error_code read_ec, write_ec, wait_ec;
        int done = 0;

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            read_ec = ec;
            ++done;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.write_some(capy::const_buffer("x", 1));
            (void)n;
            write_ec = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(reader());
        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex, ss.get_token())(waiter());
        ioc.run();

        BOOST_TEST_EQ(done, 3);
        BOOST_TEST(read_ec == capy::cond::canceled);
        BOOST_TEST(write_ec == capy::cond::canceled);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void testLocalStreamConnectAccept()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        acc.open();
        BOOST_TEST(!acc.bind(local_endpoint(path)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        ss.request_stop();

        local_stream_socket client(ioc);
        client.open();
        local_stream_socket server(ioc);

        std::error_code conn_ec, accept_ec;
        int done = 0;

        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(local_endpoint(path));
            conn_ec   = ec;
            ++done;
        };
        auto acceptor_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(server);
            accept_ec = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(connector());
        capy::run_async(ex, ss.get_token())(acceptor_task());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(conn_ec == capy::cond::canceled);
        BOOST_TEST(accept_ec == capy::cond::canceled);
        BOOST_TEST(!server.is_open());
    }

    void testLocalDatagramSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        std::error_code send_ec, recv_ec, wait_ec;
        int done = 0;

        auto send_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send(capy::const_buffer("x", 1));
            (void)n;
            send_ec = ec;
            ++done;
        };
        auto recv_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(
                capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            recv_ec = ec;
            ++done;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(send_task());
        capy::run_async(ex, ss.get_token())(recv_task());
        capy::run_async(ex, ss.get_token())(waiter());
        ioc.run();

        BOOST_TEST_EQ(done, 3);
        BOOST_TEST(send_ec == capy::cond::canceled);
        BOOST_TEST(recv_ec == capy::cond::canceled);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void testLocalDatagramSendToRecvFrom()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;

        local_datagram_socket s1(ioc), s2(ioc);
        s1.open();
        s2.open();
        BOOST_TEST(!s1.bind(local_endpoint(tmp1.path())));
        BOOST_TEST(!s2.bind(local_endpoint(tmp2.path())));

        std::stop_source ss;
        ss.request_stop();

        char buf[8];
        local_endpoint source;
        std::error_code send_to_ec, recv_from_ec;
        int done = 0;

        auto send_to_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send_to(
                capy::const_buffer("x", 1), local_endpoint(tmp2.path()));
            (void)n;
            send_to_ec = ec;
            ++done;
        };
        auto recv_from_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            (void)n;
            recv_from_ec = ec;
            ++done;
        };

        capy::run_async(ex, ss.get_token())(send_to_task());
        capy::run_async(ex, ss.get_token())(recv_from_task());
        ioc.run();

        BOOST_TEST_EQ(done, 2);
        BOOST_TEST(send_to_ec == capy::cond::canceled);
        BOOST_TEST(recv_from_ec == capy::cond::canceled);
    }

#endif // BOOST_COROSIO_POSIX

    void run()
    {
        testTcpSocket();
        testTcpConnect();
        testTcpAccept();
        testUdpSocket();
#if BOOST_COROSIO_POSIX
        testLocalStreamSocket();
        testLocalStreamConnectAccept();
        testLocalDatagramSocket();
        testLocalDatagramSendToRecvFrom();
#endif
    }
};

COROSIO_BACKEND_TESTS(precancel_test, "boost.corosio.precancel")

} // namespace boost::corosio
