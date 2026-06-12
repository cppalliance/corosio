//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/socket_option.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#endif

#include <stdexcept>
#include <system_error>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Option get/set round-trips on every socket type. The per-type I/O
// tests set options incidentally; this suite pins down the option
// plumbing itself (set_option/get_option virtuals on each backend's
// socket implementation) and the closed-socket guards.

template<auto Backend>
struct socket_option_test
{
    void testTcpOptions()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::no_delay(true));
        BOOST_TEST(sock.get_option<socket_option::no_delay>().value());
        sock.set_option(socket_option::no_delay(false));
        BOOST_TEST(!sock.get_option<socket_option::no_delay>().value());

        sock.set_option(socket_option::keep_alive(true));
        BOOST_TEST(sock.get_option<socket_option::keep_alive>().value());

        sock.set_option(socket_option::reuse_address(true));
        BOOST_TEST(sock.get_option<socket_option::reuse_address>().value());

        // Kernels round buffer sizes (Linux doubles them); only require
        // that the readback is at least what was requested.
        sock.set_option(socket_option::receive_buffer_size(16384));
        BOOST_TEST_GE(
            sock.get_option<socket_option::receive_buffer_size>().value(),
            16384);
        sock.set_option(socket_option::send_buffer_size(16384));
        BOOST_TEST_GE(
            sock.get_option<socket_option::send_buffer_size>().value(),
            16384);

        sock.set_option(socket_option::linger(true, 5));
        auto lg = sock.get_option<socket_option::linger>();
        BOOST_TEST(lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 5);

        sock.close();
    }

    // Bound-socket local_endpoint readback on every backend.
    void testTcpLocalEndpoint()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        auto ep = sock.local_endpoint();
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.v4_address() == ipv4_address::loopback());
        BOOST_TEST_GT(ep.port(), 0);

        sock.close();
    }

    void testUdpOptions()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v4());

        sock.set_option(socket_option::broadcast(true));
        BOOST_TEST(sock.get_option<socket_option::broadcast>().value());

        sock.set_option(socket_option::reuse_address(true));
        BOOST_TEST(sock.get_option<socket_option::reuse_address>().value());

        sock.set_option(socket_option::receive_buffer_size(16384));
        BOOST_TEST_GE(
            sock.get_option<socket_option::receive_buffer_size>().value(),
            16384);

        sock.close();
    }

    // The byte-sized IPv4 multicast options exercise the option
    // resize() normalization for getsockopt writing a single byte.
    void testMulticastOptions()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v4());

        sock.set_option(socket_option::multicast_loop_v4(false));
        BOOST_TEST(
            !sock.get_option<socket_option::multicast_loop_v4>().value());
        sock.set_option(socket_option::multicast_loop_v4(true));
        BOOST_TEST(
            sock.get_option<socket_option::multicast_loop_v4>().value());

        sock.set_option(socket_option::multicast_hops_v4(5));
        BOOST_TEST_EQ(
            sock.get_option<socket_option::multicast_hops_v4>().value(), 5);

        sock.close();
    }

    void testV6Only()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v6());

        sock.set_option(socket_option::v6_only(true));
        BOOST_TEST(sock.get_option<socket_option::v6_only>().value());
        sock.set_option(socket_option::v6_only(false));
        BOOST_TEST(!sock.get_option<socket_option::v6_only>().value());

        sock.close();
    }

    void testClosedSocketThrows()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        bool set_threw = false;
        try
        {
            sock.set_option(socket_option::no_delay(true));
        }
        catch (std::logic_error const&)
        {
            set_threw = true;
        }
        BOOST_TEST(set_threw);

        bool get_threw = false;
        try
        {
            (void)sock.get_option<socket_option::no_delay>();
        }
        catch (std::logic_error const&)
        {
            get_threw = true;
        }
        BOOST_TEST(get_threw);
    }

    // An option the protocol does not support reports a system error.
    void testInvalidOptionReportsError()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v4());

        bool threw = false;
        try
        {
            // TCP_NODELAY on a UDP socket.
            sock.set_option(socket_option::no_delay(true));
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);

        sock.close();
    }

#if BOOST_COROSIO_POSIX

    void testLocalStreamOptions()
    {
        io_context ioc(Backend);
        local_stream_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        s1.set_option(socket_option::receive_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::receive_buffer_size>().value(),
            16384);

        s1.set_option(socket_option::send_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::send_buffer_size>().value(),
            16384);
    }

    void testLocalDatagramOptions()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        s1.set_option(socket_option::receive_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::receive_buffer_size>().value(),
            16384);

        s1.set_option(socket_option::send_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::send_buffer_size>().value(),
            16384);
    }

#endif // BOOST_COROSIO_POSIX

    void run()
    {
        testTcpOptions();
        testTcpLocalEndpoint();
        testUdpOptions();
        testMulticastOptions();
        testV6Only();
        testClosedSocketThrows();
        testInvalidOptionReportsError();
#if BOOST_COROSIO_POSIX
        testLocalStreamOptions();
        testLocalDatagramOptions();
#endif
    }
};

COROSIO_BACKEND_TESTS(socket_option_test, "boost.corosio.socket_option")

} // namespace boost::corosio
