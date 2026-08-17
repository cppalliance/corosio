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
#include <boost/corosio/delay.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <stop_token>
#include <system_error>

#if BOOST_COROSIO_POSIX
// Raw socket creation for the assign()/release() adoption tests.
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <ws2tcpip.h> // sockaddr_in6, in6addr_loopback
#endif

#include "context.hpp"
#include "test_suite.hpp"
#include "test_utils.hpp"

namespace boost::corosio {
namespace {

using test::close_native_socket;
using test::invalid_native_socket;
using test::make_native_adoptable;
using test::make_native_socket;
using test::native_socket_valid;

// Fill a sockaddr_storage with a loopback address for `port`.
std::size_t
fill_loopback(sockaddr_storage& storage, std::uint16_t port, bool v6)
{
    storage = sockaddr_storage{};
    if (v6)
    {
        auto* sa6        = reinterpret_cast<sockaddr_in6*>(&storage);
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port   = htons(port);
        sa6->sin6_addr   = in6addr_loopback;
        return sizeof(sockaddr_in6);
    }
    auto* sa4            = reinterpret_cast<sockaddr_in*>(&storage);
    sa4->sin_family      = AF_INET;
    sa4->sin_port        = htons(port);
    sa4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sizeof(sockaddr_in);
}

// Bind to an ephemeral loopback port and report it.
bool
native_bind_loopback(native_handle_type h, bool v6, std::uint16_t& port_out)
{
    sockaddr_storage storage{};
    std::size_t len = fill_loopback(storage, 0, v6);
#if BOOST_COROSIO_HAS_IOCP
    SOCKET s = static_cast<SOCKET>(h);
    if (::bind(
            s, reinterpret_cast<sockaddr const*>(&storage),
            static_cast<int>(len)) != 0)
        return false;
    int name_len = static_cast<int>(sizeof(storage));
#else
    int s = static_cast<int>(h);
    if (::bind(
            s, reinterpret_cast<sockaddr const*>(&storage),
            static_cast<socklen_t>(len)) != 0)
        return false;
    socklen_t name_len = sizeof(storage);
#endif
    if (::getsockname(
            s, reinterpret_cast<sockaddr*>(&storage), &name_len) != 0)
        return false;
    port_out = v6
        ? ntohs(reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port)
        : ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
    return true;
}

// Send a datagram through a descriptor the library no longer owns.
bool
native_send_to_loopback(
    native_handle_type h,
    std::uint16_t      port,
    bool               v6,
    char const*        data,
    std::size_t        len)
{
    sockaddr_storage storage{};
    std::size_t addr_len = fill_loopback(storage, port, v6);
#if BOOST_COROSIO_HAS_IOCP
    return ::sendto(
               static_cast<SOCKET>(h), data, static_cast<int>(len), 0,
               reinterpret_cast<sockaddr const*>(&storage),
               static_cast<int>(addr_len)) == static_cast<int>(len);
#else
    return ::sendto(
               static_cast<int>(h), data, len, 0,
               reinterpret_cast<sockaddr const*>(&storage),
               static_cast<socklen_t>(addr_len)) ==
           static_cast<ssize_t>(len);
#endif
}

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

    void testBindClosedSocketThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        bool caught = false;
        try
        {
            auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
            (void)ec;
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testSetOptionClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        bool caught = false;
        try
        {
            sock.set_option(socket_option::broadcast(true));
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testGetOptionClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.get_option<socket_option::broadcast>();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testSendToClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        char const msg[] = "x";
        bool caught      = false;
        try
        {
            (void)sock.send_to(
                capy::const_buffer(msg, sizeof(msg)),
                endpoint(ipv4_address::loopback(), 1));
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testRecvFromClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        char buf[16];
        endpoint src;
        bool caught = false;
        try
        {
            (void)sock.recv_from(capy::mutable_buffer(buf, sizeof(buf)), src);
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testSendClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        char const msg[] = "x";
        bool caught      = false;
        try
        {
            (void)sock.send(capy::const_buffer(msg, sizeof(msg)));
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testRecvClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        char buf[16];
        bool caught = false;
        try
        {
            (void)sock.recv(capy::mutable_buffer(buf, sizeof(buf)));
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testBindAddressInUse()
    {
        io_context ioc(Backend);

        udp_socket sock1(ioc);
        sock1.open();
        auto ec = sock1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = sock1.local_endpoint().port();

        udp_socket sock2(ioc);
        sock2.open();
        ec = sock2.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(ec);

        sock1.close();
        sock2.close();
    }

    void testClosedAccessorsReturnDefaults()
    {
        // Accessors on a closed socket must not throw and must
        // return sentinel/default values.
        io_context ioc(Backend);
        udp_socket sock(ioc);

        BOOST_TEST_EQ(sock.is_open(), false);
#if BOOST_COROSIO_HAS_IOCP
        auto const invalid = static_cast<native_handle_type>(~0ull);
#else
        auto const invalid = static_cast<native_handle_type>(-1);
#endif
        BOOST_TEST_EQ(sock.native_handle(), invalid);
        BOOST_TEST(sock.local_endpoint() == endpoint{});
        BOOST_TEST(sock.remote_endpoint() == endpoint{});

        // cancel() on a closed socket is a no-op (early return).
        sock.cancel();
        BOOST_TEST_EQ(sock.is_open(), false);

        // close() on a closed socket is a no-op (early return).
        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpenIdempotent()
    {
        // Re-opening an already-open socket returns without re-creating
        // the underlying handle (early return at the head of open()).
        io_context ioc(Backend);
        udp_socket sock(ioc);

        sock.open();
        BOOST_TEST(sock.is_open());
        auto nh = sock.native_handle();

        sock.open();
        BOOST_TEST(sock.is_open());
        BOOST_TEST_EQ(sock.native_handle(), nh);

        sock.close();
    }

    void testBindNonLocalAddress()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        auto ec = sock.bind(endpoint(ipv4_address("1.2.3.4"), 0));
        BOOST_TEST(ec);

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

            (void)co_await corosio::delay(std::chrono::milliseconds(50));
            sock.cancel();

            (void)co_await corosio::delay(std::chrono::milliseconds(50));

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

            (void)co_await corosio::delay(std::chrono::milliseconds(50));
            sock.close();

            (void)co_await corosio::delay(std::chrono::milliseconds(50));

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
            auto [ec] =
                co_await corosio::delay(std::chrono::milliseconds(1000));
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
            (void)co_await corosio::delay(std::chrono::milliseconds(50));

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

    void testConnect()
    {
        io_context ioc(Backend);

        udp_socket sender(ioc);
        udp_socket receiver(ioc);

        receiver.open();
        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        sender.open();

        auto task = [](udp_socket& s, endpoint dest) -> capy::task<> {
            auto [ec] = co_await s.connect(dest);
            BOOST_TEST_EQ(ec, std::error_code{});
            BOOST_TEST_EQ(s.remote_endpoint().port(), dest.port());
            BOOST_TEST_EQ(s.remote_endpoint().v4_address(), dest.v4_address());
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, recv_ep));
        ioc.run();
    }

    void testConnectAutoOpen()
    {
        io_context ioc(Backend);

        udp_socket receiver(ioc);
        receiver.open();
        auto ec = receiver.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto recv_ep = receiver.local_endpoint();

        udp_socket sender(ioc);
        BOOST_TEST_EQ(sender.is_open(), false);

        auto task = [](udp_socket& s, endpoint dest) -> capy::task<> {
            auto [ec] = co_await s.connect(dest);
            BOOST_TEST_EQ(ec, std::error_code{});
            BOOST_TEST_EQ(s.is_open(), true);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(sender, recv_ep));
        ioc.run();
    }

    void testSendRecvConnected()
    {
        io_context ioc(Backend);

        udp_socket a(ioc);
        udp_socket b(ioc);

        b.open();
        auto ec = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto b_ep = b.local_endpoint();

        auto task = [](udp_socket& a, udp_socket& b,
                       endpoint dest) -> capy::task<> {
            // Connect a -> b
            auto [ec1] = co_await a.connect(dest);
            BOOST_TEST_EQ(ec1, std::error_code{});

            // Send via connected send
            char const msg[] = "connected";
            auto [ec2, n2] =
                co_await a.send(capy::const_buffer(msg, sizeof(msg)));
            BOOST_TEST_EQ(ec2, std::error_code{});
            BOOST_TEST_EQ(n2, sizeof(msg));

            // Receive on b (still connectionless)
            char buf[64] = {};
            endpoint source;
            auto [ec3, n3] = co_await b.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST_EQ(ec3, std::error_code{});
            BOOST_TEST_EQ(n3, sizeof(msg));
            BOOST_TEST_EQ(std::strcmp(buf, "connected"), 0);

            // Connect b -> a so we can use connected recv
            auto [ec4] = co_await b.connect(source);
            BOOST_TEST_EQ(ec4, std::error_code{});

            // Send from b, receive on a via connected recv
            char const reply[] = "reply";
            auto [ec5, n5] =
                co_await b.send(capy::const_buffer(reply, sizeof(reply)));
            BOOST_TEST_EQ(ec5, std::error_code{});

            char buf2[64] = {};
            auto [ec6, n6] =
                co_await a.recv(capy::mutable_buffer(buf2, sizeof(buf2)));
            BOOST_TEST_EQ(ec6, std::error_code{});
            BOOST_TEST_EQ(n6, sizeof(reply));
            BOOST_TEST_EQ(std::strcmp(buf2, "reply"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(a, b, b_ep));
        ioc.run();
    }

    void testSendRecvConnectedV6()
    {
        io_context ioc(Backend);

        udp_socket a(ioc);
        udp_socket b(ioc);

        b.open(udp::v6());
        auto ec = b.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto b_ep = b.local_endpoint();

        auto task = [](udp_socket& a, udp_socket& b,
                       endpoint dest) -> capy::task<> {
            auto [ec1] = co_await a.connect(dest);
            BOOST_TEST_EQ(ec1, std::error_code{});

            char const msg[] = "v6conn";
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
            BOOST_TEST_EQ(std::strcmp(buf, "v6conn"), 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(task(a, b, b_ep));
        ioc.run();
    }

    void testCancelConnectedRecv()
    {
        io_context ioc(Backend);

        udp_socket a(ioc);
        udp_socket b(ioc);

        b.open();
        auto ec = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST_EQ(ec, std::error_code{});
        auto b_ep = b.local_endpoint();

        auto task = [&]() -> capy::task<> {
            auto [ec1] = co_await a.connect(b_ep);
            BOOST_TEST_EQ(ec1, std::error_code{});

            bool recv_done = false;
            std::error_code recv_ec;

            auto nested = [&a, &recv_done, &recv_ec]() -> capy::task<> {
                char buf[64];
                auto [ec, n] =
                    co_await a.recv(capy::mutable_buffer(buf, sizeof(buf)));
                recv_ec   = ec;
                recv_done = true;
            };
            capy::run_async(ioc.get_executor())(nested());

            (void)co_await corosio::delay(std::chrono::milliseconds(50));
            a.cancel();

            (void)co_await corosio::delay(std::chrono::milliseconds(50));

            BOOST_TEST(recv_done);
            BOOST_TEST(recv_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
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

        // Join may fail with an environment-specific error in CI
        // without multicast routing; skip the rest of the test.
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
            // CI runners may lack a multicast route (EHOSTUNREACH)
            if (ec1)
                co_return;

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

        receiver.close();
        sender.close();
    }

    // Multicast set_option tests below accept any std::system_error.
    // Multicast routing is environment-specific: we've observed at
    // least EADDRNOTAVAIL (Linux containers), EINVAL (BSD strict
    // RFC 3493 reading), and ENODEV (interface-scoped without scope id)
    // across CI runs. The library code path is exercised either way;
    // these tests are for coverage of set_option, not for asserting
    // that the kernel completes the request.

    void testMulticastLeaveV4()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        try
        {
            sock.set_option(
                socket_option::join_group_v4(ipv4_address("239.255.0.2")));
            sock.set_option(
                socket_option::leave_group_v4(ipv4_address("239.255.0.2")));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        sock.close();
    }

    void testMulticastJoinLeaveV6()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v6());

        try
        {
            sock.set_option(
                socket_option::join_group_v6(ipv6_address("ff02::1")));
            sock.set_option(
                socket_option::leave_group_v6(ipv6_address("ff02::1")));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        sock.close();
    }

    void testMulticastInterfaceV4()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        try
        {
            sock.set_option(
                socket_option::multicast_interface_v4(ipv4_address::any()));
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        sock.close();
    }

    void testMulticastInterfaceV6()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open(udp::v6());

        try
        {
            sock.set_option(socket_option::multicast_interface_v6(0));
            auto opt = sock.get_option<socket_option::multicast_interface_v6>();
            BOOST_TEST_EQ(opt.value(), 0);
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        sock.close();
    }

    void testBufferSizeBoundary()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        // Linux clamps SO_RCVBUF=0 to a minimum and reports success;
        // BSD platforms (macOS, FreeBSD) reject 0 with EINVAL.
        try
        {
            sock.set_option(socket_option::receive_buffer_size(0));
            int rcv =
                sock.get_option<socket_option::receive_buffer_size>().value();
            // Linux clamps to a minimum > 0; Windows allows 0.
            BOOST_TEST(rcv >= 0);
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        try
        {
            sock.set_option(socket_option::send_buffer_size(0));
            int snd =
                sock.get_option<socket_option::send_buffer_size>().value();
            // Linux clamps to a minimum > 0; Windows allows 0.
            BOOST_TEST(snd >= 0);
        }
        catch (std::system_error const&)
        {
            BOOST_TEST_PASS();
        }

        // Large value: set succeeds on every platform, but the kernel
        // may clamp to net.core.rmem_max (often a few hundred KiB in
        // constrained containers). Only assert non-zero.
        sock.set_option(socket_option::receive_buffer_size(1024 * 1024));
        int rcv = sock.get_option<socket_option::receive_buffer_size>().value();
        BOOST_TEST(rcv > 0);

        sock.close();
    }

    void testWrongProtocolNoDelayOnUdp()
    {
        // TCP_NODELAY is meaningful only on TCP; setting on UDP must error.
        io_context ioc(Backend);
        udp_socket sock(ioc);
        sock.open();

        bool caught = false;
        try
        {
            sock.set_option(socket_option::no_delay(true));
        }
        catch (std::system_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);

        sock.close();
    }

    // Adopt a natively created bound datagram socket and round-trip a
    // datagram through the library in both directions.
    void testAssignBoundSocket()
    {
        io_context ioc(Backend);

        udp_socket peer(ioc);
        peer.open();
        auto ec = peer.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto peer_ep = peer.local_endpoint();

        auto nfd = make_native_socket(AF_INET, SOCK_DGRAM);
        BOOST_TEST(nfd != invalid_native_socket);
        std::uint16_t nport = 0;
        BOOST_TEST(native_bind_loopback(nfd, false, nport));
        make_native_adoptable(nfd);

        udp_socket adopted(ioc);
        adopted.assign(nfd);
        BOOST_TEST(adopted.is_open());
        BOOST_TEST(adopted.native_handle() == nfd);
        BOOST_TEST_EQ(adopted.local_endpoint().port(), nport);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            char const msg[] = "adopted";
            auto [ec1, n1]   = co_await adopted.send_to(
                capy::const_buffer(msg, sizeof(msg)), peer_ep);
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, sizeof(msg));

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await peer.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, sizeof(msg));
            BOOST_TEST_EQ(source.port(), nport);

            // Reverse direction through the adopted socket.
            char const reply[] = "back";
            auto [ec3, n3]     = co_await peer.send_to(
                capy::const_buffer(reply, sizeof(reply)), source);
            BOOST_TEST(!ec3);
            (void)n3;

            char buf2[64] = {};
            endpoint from;
            auto [ec4, n4] = co_await adopted.recv_from(
                capy::mutable_buffer(buf2, sizeof(buf2)), from);
            BOOST_TEST(!ec4);
            BOOST_TEST_EQ(n4, sizeof(reply));
            done = (std::strcmp(buf2, "back") == 0);
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    // Rejection matrix: bad handle, wrong type, wrong family, self.
    void testAssignRejections()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        auto expect_throw = [&](native_handle_type h) {
            bool threw = false;
            try
            {
                sock.assign(h);
            }
            catch (std::system_error const&)
            {
                threw = true;
            }
            BOOST_TEST(threw);
        };

        expect_throw(invalid_native_socket);
        BOOST_TEST(!sock.is_open());

        auto st = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(st != invalid_native_socket);
        expect_throw(st);
        BOOST_TEST(native_socket_valid(st)); // caller keeps it
        close_native_socket(st);

#if BOOST_COROSIO_POSIX
        auto un = make_native_socket(AF_UNIX, SOCK_DGRAM);
        BOOST_TEST(un != invalid_native_socket);
        expect_throw(un);
        BOOST_TEST(native_socket_valid(un));
        close_native_socket(un);
#endif

        sock.open(udp::v4());
        expect_throw(sock.native_handle());
        BOOST_TEST(sock.is_open());
        sock.close();
    }

    // A failed assign over an open socket leaves it functional.
    void testAssignFailureKeepsSocket()
    {
        io_context ioc(Backend);

        udp_socket peer(ioc);
        peer.open();
        auto ec = peer.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto peer_ep = peer.local_endpoint();

        udp_socket sock(ioc);
        sock.open();
        ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto before = sock.native_handle();

        auto st = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(st != invalid_native_socket);
        bool threw = false;
        try
        {
            sock.assign(st);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
        BOOST_TEST(native_socket_valid(st));
        close_native_socket(st);

        BOOST_TEST(sock.is_open());
        BOOST_TEST(sock.native_handle() == before);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            char const msg[] = "intact";
            auto [ec1, n1]   = co_await sock.send_to(
                capy::const_buffer(msg, sizeof(msg)), peer_ep);
            BOOST_TEST(!ec1);

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await peer.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST(!ec2);
            done = (n2 == n1);
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
    }

    // Assign over an open socket cancels its pending operations and
    // leaves the adopted descriptor usable.
    void testAssignOverOpenCancelsPending()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket peer(ioc);
        peer.open();
        auto ec = peer.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto peer_ep = peer.local_endpoint();

        udp_socket sock(ioc);
        sock.open();
        ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        auto nfd = make_native_socket(AF_INET, SOCK_DGRAM);
        BOOST_TEST(nfd != invalid_native_socket);
        std::uint16_t nport = 0;
        BOOST_TEST(native_bind_loopback(nfd, false, nport));
        make_native_adoptable(nfd);

        std::error_code recv_ec;
        bool recv_done = false;
        bool delivered = false;
        char buf[64];
        endpoint source;

        auto receiver = [&]() -> capy::task<> {
            auto [rec, rn] = co_await sock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            (void)rn;
            recv_ec   = rec;
            recv_done = true;
        };
        auto adopter = [&]() -> capy::task<> {
            sock.assign(nfd);
            char const msg[] = "after";
            auto [ec1, n1]   = co_await sock.send_to(
                capy::const_buffer(msg, sizeof(msg)), peer_ep);
            BOOST_TEST(!ec1);

            char in[64] = {};
            endpoint from;
            auto [ec2, n2] = co_await peer.recv_from(
                capy::mutable_buffer(in, sizeof(in)), from);
            BOOST_TEST(!ec2);
            delivered = (n2 == n1 && from.port() == nport);
        };

        // run_async runs inline to the first suspend, so spawning the
        // receiver first is what guarantees the recv is parked when
        // assign() runs.
        capy::run_async(ex)(receiver());
        capy::run_async(ex)(adopter());
        ioc.run();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
        BOOST_TEST(sock.is_open());
        BOOST_TEST(sock.native_handle() == nfd);
        BOOST_TEST(delivered);
    }

    // release() hands ownership to the caller only after pending
    // operations have been cancelled, and the descriptor is still
    // bound to its port.
    void testRelease()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket peer(ioc);
        peer.open();
        auto ec = peer.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto peer_port = peer.local_endpoint().port();

        udp_socket sock(ioc);
        sock.open();
        ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto sock_port = sock.local_endpoint().port();

        std::error_code recv_ec;
        bool recv_done = false;
        char buf[64];
        endpoint source;
        auto released = invalid_native_socket;

        auto receiver = [&]() -> capy::task<> {
            auto [rec, rn] = co_await sock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            (void)rn;
            recv_ec   = rec;
            recv_done = true;
        };
        auto releaser = [&]() -> capy::task<> {
            released = sock.release();
            co_return;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(releaser());
        ioc.run();
        ioc.restart();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
        BOOST_TEST(!sock.is_open());
        BOOST_TEST(released != invalid_native_socket);

        char const msg[] = "released";
        BOOST_TEST(native_send_to_loopback(
            released, peer_port, false, msg, sizeof(msg)));

        bool got = false;
        auto peeker = [&]() -> capy::task<> {
            char in[64] = {};
            endpoint from;
            auto [rec, rn] = co_await peer.recv_from(
                capy::mutable_buffer(in, sizeof(in)), from);
            BOOST_TEST(!rec);
            got = (rn == sizeof(msg) && from.port() == sock_port);
        };
        capy::run_async(ex)(peeker());
        ioc.run();
        BOOST_TEST(got);

        close_native_socket(released);
    }

    void testReleaseClosedThrows()
    {
        io_context ioc(Backend);
        udp_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.release();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    // v6 adoption: the cached endpoint must report v6.
    void testAssignV6()
    {
        io_context ioc(Backend);

        udp_socket peer(ioc);
        peer.open(udp::v6());
        auto ec = peer.bind(endpoint(ipv6_address::loopback(), 0));
        if (ec)
            return; // no IPv6 loopback on this host
        auto peer_ep = peer.local_endpoint();

        auto nfd = make_native_socket(AF_INET6, SOCK_DGRAM);
        if (nfd == invalid_native_socket)
            return;
        std::uint16_t nport = 0;
        if (!native_bind_loopback(nfd, true, nport))
        {
            close_native_socket(nfd);
            return;
        }
        make_native_adoptable(nfd);

        udp_socket adopted(ioc);
        adopted.assign(nfd);
        BOOST_TEST(adopted.is_open());
        BOOST_TEST(adopted.local_endpoint().is_v6());
        BOOST_TEST_EQ(adopted.local_endpoint().port(), nport);

        bool done = false;
        auto task = [&]() -> capy::task<> {
            char const msg[] = "v6adopt";
            auto [ec1, n1]   = co_await adopted.send_to(
                capy::const_buffer(msg, sizeof(msg)), peer_ep);
            BOOST_TEST(!ec1);

            char buf[64] = {};
            endpoint source;
            auto [ec2, n2] = co_await peer.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            BOOST_TEST(!ec2);
            BOOST_TEST(source.is_v6());
            done = (n2 == n1);
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(done);
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
        testBindClosedSocketThrows();
        testSetOptionClosedThrows();
        testGetOptionClosedThrows();
        testSendToClosedThrows();
        testRecvFromClosedThrows();
        testSendClosedThrows();
        testRecvClosedThrows();
        testBindAddressInUse();
        testBindNonLocalAddress();
        testClosedAccessorsReturnDefaults();
        testOpenIdempotent();
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
        testConnect();
        testConnectAutoOpen();
        testSendRecvConnected();
        testSendRecvConnectedV6();
        testCancelConnectedRecv();
        testMulticastLoopHops();
        testMulticastLoopHopsV6();
        testMulticastJoinV4();
        testMulticastLeaveV4();
        testMulticastJoinLeaveV6();
        testMulticastInterfaceV4();
        testMulticastInterfaceV6();
        testBufferSizeBoundary();
        testWrongProtocolNoDelayOnUdp();

        // Adoption and release
        testAssignBoundSocket();
        testAssignRejections();
        testAssignFailureKeepsSocket();
        testAssignOverOpenCancelsPending();
        testRelease();
        testReleaseClosedThrows();
        testAssignV6();
    }
};

COROSIO_BACKEND_TESTS(udp_socket_test, "boost.corosio.udp_socket")

} // namespace
} // namespace boost::corosio
