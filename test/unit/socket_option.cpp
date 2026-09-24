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

#include <boost/corosio/native/native_socket_option.hpp>

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

#include <cstring>
#include <stdexcept>
#include <system_error>
#include <tuple>

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
    // The protocol tags speak the same portable family the options
    // consume
    void testProtocolTagFamily()
    {
        static_assert(tcp::v4().family() == family::v4);
        static_assert(tcp::v6().family() == family::v6);
        static_assert(udp::v4().family() == family::v4);
        static_assert(udp::v6().family() == family::v6);
        BOOST_TEST(tcp::v6().is_v6());
        BOOST_TEST(!udp::v4().is_v6());
    }

    void testTcpOptions()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        BOOST_TEST(!sock.open());

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
            sock.get_option<socket_option::send_buffer_size>().value(), 16384);

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
        BOOST_TEST(!sock.open());

        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        auto ep = sock.local_endpoint();
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.address().to_v4() == ipv4_address::loopback());
        BOOST_TEST_GT(ep.port(), 0);

        sock.close();
    }

    void testUdpOptions()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        BOOST_TEST(!sock.open(udp::v4()));

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

    // One option type serves both families: the socket's family
    // selects the wire rendering (byte at IPPROTO_IP for v4, int at
    // IPPROTO_IPV6 for v6), verified through real setsockopt and a
    // getsockopt round-trip.
    void testMulticastOptions()
    {
        io_context ioc(Backend);

        // IPv4 rendering
        {
            udp_socket sock(ioc);
            BOOST_TEST(!sock.open(udp::v4()));

            sock.set_option(socket_option::multicast_loop(false));
            BOOST_TEST(
                !sock.get_option<socket_option::multicast_loop>().value());
            sock.set_option(socket_option::multicast_loop(true));
            BOOST_TEST(
                sock.get_option<socket_option::multicast_loop>().value());

            sock.set_option(socket_option::multicast_hops(5));
            BOOST_TEST_EQ(
                sock.get_option<socket_option::multicast_hops>().value(), 5);

            sock.close();
        }

        // IPv6 rendering, same option types
        {
            udp_socket sock(ioc);
            BOOST_TEST(!sock.open(udp::v6()));

            sock.set_option(socket_option::multicast_loop(false));
            BOOST_TEST(
                !sock.get_option<socket_option::multicast_loop>().value());
            sock.set_option(socket_option::multicast_loop(true));
            BOOST_TEST(
                sock.get_option<socket_option::multicast_loop>().value());

            sock.set_option(socket_option::multicast_hops(7));
            BOOST_TEST_EQ(
                sock.get_option<socket_option::multicast_hops>().value(), 7);

            sock.close();
        }

        // The per-family wire widths, pinned as values: a byte at the
        // IPv4 level (BSD-derived kernels reject the four-byte form),
        // an int at the IPv6 level
        {
            socket_option::multicast_loop ml(true);
            BOOST_TEST_EQ(ml.size(family::v4), 1u);
            BOOST_TEST_EQ(ml.size(family::v6), sizeof(int));
            BOOST_TEST(ml.data(family::v4) != ml.data(family::v6));

            socket_option::multicast_hops mh(9);
            BOOST_TEST_EQ(mh.size(family::v4), 1u);
            BOOST_TEST_EQ(mh.size(family::v6), sizeof(int));
            BOOST_TEST(mh.data(family::v4) != mh.data(family::v6));

            native_socket_option::multicast_loop nl(true);
            BOOST_TEST_EQ(nl.size(family::v4), 1u);
            BOOST_TEST_EQ(nl.size(family::v6), sizeof(int));
        }

        // Hop counts the IPv4 wire cannot carry are refused
        BOOST_TEST_THROWS(socket_option::multicast_hops(256), std::logic_error);
        BOOST_TEST_THROWS(socket_option::multicast_hops(-1), std::logic_error);
        BOOST_TEST_THROWS(
            native_socket_option::multicast_hops(256), std::logic_error);
    }

    // Membership traits, exercised directly so they are covered on
    // hosts with no multicast route to join. Membership dispatches
    // on the group's family, not the socket's: a v4 group renders at
    // the IPv4 level even when the family argument says v6 (the
    // dual-stack case).
    void testMembershipTraits()
    {
        socket_option::join_group join4(ip_address("239.1.2.3"));
        socket_option::leave_group leave4(ip_address("239.1.2.3"));
        socket_option::join_group join6(ip_address("ff02::1"));
        socket_option::leave_group leave6(ipv6_address("ff02::1"), 0);

        // Group family decides; the argument does not
        BOOST_TEST_EQ(join4.level(family::v4), join4.level(family::v6));
        BOOST_TEST_EQ(join6.level(family::v4), join6.level(family::v6));
        BOOST_TEST(join4.level(family::v4) != join6.level(family::v4));

        // Join and leave share levels and sizes, not names
        BOOST_TEST_EQ(leave4.level(family::v4), join4.level(family::v4));
        BOOST_TEST_EQ(leave6.level(family::v6), join6.level(family::v6));
        BOOST_TEST(leave4.name(family::v4) != join4.name(family::v4));
        BOOST_TEST(leave6.name(family::v6) != join6.name(family::v6));
        BOOST_TEST_EQ(leave4.size(family::v4), join4.size(family::v4));
        BOOST_TEST_EQ(leave6.size(family::v6), join6.size(family::v6));

        // Distinct wire structs per family
        BOOST_TEST(join4.size(family::v4) != join6.size(family::v4));

        // The public vocabulary is bytewise the native one, for every
        // object and either family argument
        auto same_bytes = [](auto const& pub, auto const& nat) {
            for (auto f : {family::v4, family::v6})
            {
                BOOST_TEST_EQ(pub.level(f), nat.level(f));
                BOOST_TEST_EQ(pub.name(f), nat.name(f));
                if (!BOOST_TEST_EQ(pub.size(f), nat.size(f)))
                    continue;
                BOOST_TEST(
                    std::memcmp(pub.data(f), nat.data(f), pub.size(f)) == 0);
            }
        };
        same_bytes(
            join4, native_socket_option::join_group(ip_address("239.1.2.3")));
        same_bytes(
            leave4, native_socket_option::leave_group(ip_address("239.1.2.3")));
        same_bytes(
            join6, native_socket_option::join_group(ip_address("ff02::1")));
        same_bytes(
            leave6,
            native_socket_option::leave_group(ipv6_address("ff02::1"), 0));

        // A v6 group's zone is the default interface index; an explicit
        // index outranks it
        {
            native_socket_option::join_group zoned(ip_address("ff02::1%2"));
            auto const* m =
                static_cast<struct ipv6_mreq const*>(zoned.data(family::v6));
            BOOST_TEST_EQ(m->ipv6mr_interface, 2u);

            native_socket_option::join_group unzoned(ip_address("ff02::1"));
            m = static_cast<struct ipv6_mreq const*>(unzoned.data(family::v6));
            BOOST_TEST_EQ(m->ipv6mr_interface, 0u);

            native_socket_option::join_group explicit_index(
                ipv6_address("ff02::1%2"), 5);
            m = static_cast<struct ipv6_mreq const*>(
                explicit_index.data(family::v6));
            BOOST_TEST_EQ(m->ipv6mr_interface, 5u);
        }
    }

    // The interface option's IPv4 rendering, pinned with a non-any
    // address so an empty marshal cannot pass
    void testMulticastInterfaceRendering()
    {
        ipv4_address const iface("192.168.7.9");

        socket_option::multicast_interface pub(iface);
        BOOST_TEST_EQ(pub.address(), iface);
        BOOST_TEST_EQ(pub.if_index(), 0u);

        native_socket_option::multicast_interface nat(iface);
        BOOST_TEST_EQ(nat.address(), iface);
        BOOST_TEST_EQ(pub.size(family::v4), nat.size(family::v4));
        BOOST_TEST(
            std::memcmp(
                pub.data(family::v4), nat.data(family::v4),
                pub.size(family::v4)) == 0);

        BOOST_TEST_EQ(socket_option::multicast_interface(5u).if_index(), 5u);
        BOOST_TEST_EQ(
            native_socket_option::multicast_interface(5u).if_index(), 5u);
    }

    void testV6Only()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        BOOST_TEST(!sock.open(udp::v6()));

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

        std::error_code set_caught;
        try
        {
            sock.set_option(socket_option::no_delay(true));
        }
        catch (std::system_error const& e)
        {
            set_caught = e.code();
        }
        BOOST_TEST(set_caught == std::errc::bad_file_descriptor);

        std::error_code get_caught;
        try
        {
            std::ignore = sock.get_option<socket_option::no_delay>();
        }
        catch (std::system_error const& e)
        {
            get_caught = e.code();
        }
        BOOST_TEST(get_caught == std::errc::bad_file_descriptor);
    }

    // An option the protocol does not support reports a system error.
    void testInvalidOptionReportsError()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        BOOST_TEST(!sock.open(udp::v4()));

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
            s1.get_option<socket_option::receive_buffer_size>().value(), 16384);

        s1.set_option(socket_option::send_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::send_buffer_size>().value(), 16384);
    }

    void testLocalDatagramOptions()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        s1.set_option(socket_option::receive_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::receive_buffer_size>().value(), 16384);

        s1.set_option(socket_option::send_buffer_size(16384));
        BOOST_TEST_GE(
            s1.get_option<socket_option::send_buffer_size>().value(), 16384);
    }

#endif // BOOST_COROSIO_POSIX

    // getsockopt writes a single byte for boolean/integer options on some
    // platforms; resize() normalizes the stored value afterwards. Drive it
    // directly so the single-byte branch is covered on hosts whose
    // getsockopt writes the full width.
    void testResizeNormalization()
    {
        socket_option::no_delay b(true);
        b.resize(family::v4, 1);
        BOOST_TEST(b.value());

        socket_option::receive_buffer_size i(1);
        i.resize(family::v4, 1);
        BOOST_TEST_EQ(i.value(), 1);
    }

    void run()
    {
        testResizeNormalization();
        testProtocolTagFamily();
        testTcpOptions();
        testTcpLocalEndpoint();
        testUdpOptions();
        testMulticastOptions();
        testMembershipTraits();
        testMulticastInterfaceRendering();
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
