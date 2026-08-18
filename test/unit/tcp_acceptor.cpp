//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tcp_acceptor.hpp>

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <system_error>

#ifndef _WIN32
// For the SO_REUSEPORT guard around testReusePort. The corosio public
// option header is platform-agnostic and does not expose this macro.
// netinet/in.h and unistd.h support the raw-socket backlog setup in
// testAcceptPendingConnection; fcntl.h supports the adoption tests.
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
// Raw-socket backlog setup in testAcceptPendingConnection.
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

// Build the listening descriptor a socket-activation supervisor would
// hand over: bound, listening, and already in the mode the backend
// needs. Returns invalid_native_socket when the family is unusable.
native_handle_type
make_native_listener(bool v6, std::uint16_t& port)
{
    auto h = make_native_socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM);
    if (h == invalid_native_socket)
        return h;

    sockaddr_storage storage{};
    auto len = fill_loopback(storage, 0, v6);
#if BOOST_COROSIO_HAS_IOCP
    SOCKET s = static_cast<SOCKET>(h);
    if (::bind(s, reinterpret_cast<sockaddr const*>(&storage),
            static_cast<int>(len)) != 0 ||
        ::listen(s, 4) != 0)
    {
        close_native_socket(h);
        return invalid_native_socket;
    }
    int name_len = static_cast<int>(sizeof(storage));
#else
    int s = static_cast<int>(h);
    if (::bind(s, reinterpret_cast<sockaddr const*>(&storage),
            static_cast<socklen_t>(len)) != 0 ||
        ::listen(s, 4) != 0)
    {
        close_native_socket(h);
        return invalid_native_socket;
    }
    socklen_t name_len = sizeof(storage);
#endif
    storage = sockaddr_storage{};
    if (::getsockname(
            s, reinterpret_cast<sockaddr*>(&storage), &name_len) != 0)
    {
        close_native_socket(h);
        return invalid_native_socket;
    }
    port = v6
        ? ntohs(reinterpret_cast<sockaddr_in6 const*>(&storage)->sin6_port)
        : ntohs(reinterpret_cast<sockaddr_in const*>(&storage)->sin_port);

    make_native_adoptable(h);
    return h;
}

// Blocking connect: on loopback the handshake completes against the
// listen backlog without the io_context running.
bool
native_connect_loopback(native_handle_type h, std::uint16_t port, bool v6)
{
    sockaddr_storage storage{};
    auto len = fill_loopback(storage, port, v6);
#if BOOST_COROSIO_HAS_IOCP
    return ::connect(
               static_cast<SOCKET>(h),
               reinterpret_cast<sockaddr const*>(&storage),
               static_cast<int>(len)) == 0;
#else
    return ::connect(
               static_cast<int>(h),
               reinterpret_cast<sockaddr const*>(&storage),
               static_cast<socklen_t>(len)) == 0;
#endif
}

// Take ownership of a released listener the way a caller would: clear
// the non-blocking flag the library set, then accept.
native_handle_type
native_accept_blocking(native_handle_type h)
{
#if BOOST_COROSIO_HAS_IOCP
    return static_cast<native_handle_type>(
        ::accept(static_cast<SOCKET>(h), nullptr, nullptr));
#else
    int fd    = static_cast<int>(h);
    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return static_cast<native_handle_type>(::accept(fd, nullptr, nullptr));
#endif
}

} // namespace

// Acceptor-specific tests
// Focus: acceptor construction, basic interface, and cancellation
//
// Tests are templated on the context type to run with all available backends.

template<auto Backend>
struct tcp_acceptor_test
{
    void testConstruction()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Acceptor should not be open initially
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void testListen()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(acc.is_open(), true);

        // Close it
        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void testOptions()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto opt = acc.get_option<socket_option::reuse_address>();
        BOOST_TEST(opt.value());
        acc.close();

        tcp_acceptor closed(ioc);
        BOOST_TEST_THROWS(
            closed.set_option(socket_option::reuse_address(true)),
            std::logic_error);
        BOOST_TEST_THROWS(
            (void)closed.get_option<socket_option::reuse_address>(),
            std::logic_error);
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        tcp_acceptor acc1(ioc);
        acc1.open();
        acc1.set_option(socket_option::reuse_address(true));
        auto ec = acc1.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc1.listen();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(acc1.is_open(), true);

        // Move construct
        tcp_acceptor acc2(std::move(acc1));
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        tcp_acceptor acc1(ioc);
        tcp_acceptor acc2(ioc);
        acc1.open();
        acc1.set_option(socket_option::reuse_address(true));
        auto ec = acc1.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc1.listen();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(acc1.is_open(), true);
        BOOST_TEST_EQ(acc2.is_open(), false);

        // Move assign
        acc2 = std::move(acc1);
        BOOST_TEST_EQ(acc1.is_open(), false);
        BOOST_TEST_EQ(acc2.is_open(), true);

        acc2.close();
    }

    // Cancellation Tests

    void testCancelAccept()
    {
        // Tests that cancel() properly cancels a pending accept operation.
        // This exercises the acceptor_ptr shared_ptr that keeps the
        // acceptor impl alive until IOCP delivers the cancellation.
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        // These must outlive the coroutines
        bool accept_done = false;
        std::error_code accept_ec;
        tcp_socket peer(ioc);

        auto task = [&]() -> capy::task<> {
            // Launch accept that will block (no incoming connections)
            // Store lambda in variable to ensure it outlives the coroutine.
            auto nested_coro = [&acc, &peer, &accept_done,
                                &accept_ec]() -> capy::task<> {
                auto [ec]   = co_await acc.accept(peer);
                accept_ec   = ec;
                accept_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait then cancel
            (void)co_await corosio::delay(std::chrono::milliseconds(50));
            acc.cancel();

            // Wait for accept to complete
            (void)co_await corosio::delay(std::chrono::milliseconds(50));

            BOOST_TEST(accept_done);
            BOOST_TEST(accept_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        acc.close();
    }

    // Destroy the io_context with a read still parked on a connected
    // socket; service shutdown must drain the abandoned operation
    // without resuming it.
    void testDestroyWithParkedRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        // Bind to loopback explicitly: connecting to a wildcard-bound
        // listener's 0.0.0.0 address only works on some platforms.
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto ep = endpoint(
            ipv4_address::loopback(), acc.local_endpoint().port());

        tcp_socket server(ioc);
        tcp_socket client(ioc);

        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s) -> capy::task<> {
                (void)co_await a.accept(s);
            }(acc, server));
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint e) -> capy::task<> {
                (void)co_await s.connect(e);
            }(client, ep));
        ioc.run();
        BOOST_TEST(server.is_open());
        BOOST_TEST(client.is_open());

        ioc.restart();

        char buf[16];
        auto reader = [&]() -> capy::task<> {
            (void)co_await server.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
        };
        capy::run_async(ex)(reader());

        (void)ioc.run_one();
        BOOST_TEST_PASS();
    }

    void testCloseWhilePendingAccept()
    {
        // Tests that close() properly handles a pending accept operation.
        // This is the key test for the cancel/destruction race condition:
        // when close() is called, CancelIoEx is invoked, the tcp_socket is closed,
        // but the impl must stay alive until IOCP delivers the cancellation.
        // The acceptor_ptr shared_ptr in accept_op ensures this.
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        tcp_socket peer(ioc);
        bool accept_done = false;
        std::error_code accept_ec;

        // Pattern from tcp_socket tests: run a single coroutine that manages
        // the nested coroutine and close operation
        auto task = [&ioc, &acc, &peer, &accept_done,
                     &accept_ec]() -> capy::task<> {
            // Store lambda in variable to ensure it outlives the coroutine.
            // Lambda coroutines capture 'this' by reference, so the lambda
            // must remain alive while the coroutine is suspended.
            auto nested_coro = [&acc, &peer, &accept_done,
                                &accept_ec]() -> capy::task<> {
                auto [ec]   = co_await acc.accept(peer);
                accept_ec   = ec;
                accept_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait then close the acceptor
            (void)co_await corosio::delay(std::chrono::milliseconds(50));
            acc.close();

            (void)co_await corosio::delay(std::chrono::milliseconds(50));

            BOOST_TEST(accept_done);
            BOOST_TEST(accept_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
    }

    void testListenV6()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST(acc.local_endpoint().is_v6());
        BOOST_TEST(acc.local_endpoint().port() != 0);

        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    void testAcceptV6()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, peer, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv6_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        // Both endpoints should be IPv6
        BOOST_TEST(peer.local_endpoint().is_v6());
        BOOST_TEST(peer.remote_endpoint().is_v6());

        peer.close();
        client.close();
        acc.close();
    }

    void testAcceptReturning()
    {
        // Returning overload: accept() yields the peer socket directly,
        // associated with the acceptor's execution context.
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket client(ioc);

        bool accept_done    = false;
        bool connect_done   = false;
        bool peer_local_v6  = false;
        bool peer_remote_v6 = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, std::error_code& ec_out, bool& done,
               bool& local_v6, bool& remote_v6) -> capy::task<> {
                auto [ec, peer] = co_await a.accept();
                ec_out          = ec;
                if (!ec)
                {
                    local_v6  = peer.local_endpoint().is_v6();
                    remote_v6 = peer.remote_endpoint().is_v6();
                }
                done = true;
            }(acc, accept_ec, accept_done, peer_local_v6, peer_remote_v6));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv6_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST(peer_local_v6);
        BOOST_TEST(peer_remote_v6);

        client.close();
        acc.close();
    }

    void testDualStackAccept()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Default v6only=false gives dual-stack
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::any(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, peer, accept_ec, accept_done));

        // Connect with IPv4 client to the dual-stack listener
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        // Peer remote endpoint is IPv6 (IPv4-mapped)
        BOOST_TEST(peer.remote_endpoint().is_v6());

        peer.close();
        client.close();
        acc.close();
    }

    void testV6OnlyAccept()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Explicit v6only restricts to IPv6
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        acc.set_option(socket_option::v6_only(true));
        auto ec = acc.bind(endpoint(ipv6_address::any(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        bool connect_done = false;
        std::error_code connect_ec;

        auto ex = ioc.get_executor();

        // IPv4 connect should be refused
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        // Cancel lingering accept after connect completes
        auto cancel_task = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(200));
            acc.cancel();
        };
        capy::run_async(ex)(cancel_task());

        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(connect_ec); // Should fail (connection refused)

        acc.close();
        client.close();
    }

    void testOpenThenListen()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        BOOST_TEST_EQ(acc.is_open(), true);

        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST(acc.local_endpoint().port() != 0);

        // Accept a connection to verify the acceptor works
        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto port = acc.local_endpoint().port();
        auto ex   = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, peer, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        peer.close();
        client.close();
        acc.close();
    }

#ifdef SO_REUSEPORT
    void testReusePort()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        acc.set_option(socket_option::reuse_port(true));

        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST(acc.local_endpoint().port() != 0);

        // Verify the option took effect
        auto opt = acc.get_option<socket_option::reuse_port>();
        BOOST_TEST(opt.value());

        acc.close();
    }
#endif

    void testOpenIdempotent()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        BOOST_TEST_EQ(acc.is_open(), true);

        // Second open should be a no-op
        acc.open();
        BOOST_TEST_EQ(acc.is_open(), true);

        acc.close();
    }

    void testConvenienceConstructor()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc, endpoint(0));

        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST(acc.local_endpoint().port() != 0);

        acc.close();
    }

    void testConvenienceConstructorIPv6()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc, endpoint(ipv6_address::loopback(), 0));

        BOOST_TEST_EQ(acc.is_open(), true);
        BOOST_TEST(acc.local_endpoint().is_v6());
        BOOST_TEST(acc.local_endpoint().port() != 0);

        acc.close();
    }

    void testBindThenListen()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        auto port = acc.local_endpoint().port();
        BOOST_TEST(port != 0);

        // Verify by accepting a connection
        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, peer, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(client, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        peer.close();
        client.close();
        acc.close();
    }

    void testBindClosedAcceptorThrows()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        bool caught = false;
        try
        {
            auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
            (void)ec;
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

        tcp_acceptor acc1(ioc);
        acc1.open();
        acc1.set_option(socket_option::reuse_address(true));
        auto ec = acc1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = acc1.local_endpoint().port();

        tcp_acceptor acc2(ioc);
        acc2.open();
        ec = acc2.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(ec);

        acc1.close();
        acc2.close();
    }

    void testBindError()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        acc.open();

        // Bind to an address not assigned to any local interface
        auto ec = acc.bind(endpoint(ipv4_address("1.2.3.4"), 0));
        BOOST_TEST(ec);

        acc.close();
    }

    void testListenClosedThrows()
    {
        // listen() on a closed acceptor throws std::logic_error.
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        bool caught = false;
        try
        {
            auto ec = acc.listen();
            (void)ec;
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testClosedAcceptorAccessors()
    {
        // cancel() and local_endpoint() on a closed acceptor must
        // return without throwing (early return on !is_open()).
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        BOOST_TEST_EQ(acc.is_open(), false);

        acc.cancel();
        BOOST_TEST_EQ(acc.is_open(), false);

        BOOST_TEST(acc.local_endpoint() == endpoint{});

        // close() on a closed acceptor is a no-op.
        acc.close();
        BOOST_TEST_EQ(acc.is_open(), false);
    }

    // accept()/wait() on a closed acceptor must throw rather than
    // initiate an operation on an invalid handle.
    void testClosedAcceptorOpsThrow()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        tcp_socket peer(ioc);

        auto expect_throw = [](auto fn) {
            bool threw = false;
            try
            {
                fn();
            }
            catch (std::logic_error const&)
            {
                threw = true;
            }
            BOOST_TEST(threw);
        };

        expect_throw([&] { (void)acc.accept(peer); });
        expect_throw([&] { (void)acc.accept(); });
        expect_throw([&] { (void)acc.wait(wait_type::read); });
    }

    // Stop-token cancel of a parked accept. Unlike testCancelAccept
    // (acceptor-wide cancel()), this routes through the per-waiter
    // stop callback.
    void testStopTokenAccept()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        std::stop_source ss;
        tcp_socket peer(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(peer);
            accept_ec   = aec;
            accept_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(20));
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(accept_ec == capy::cond::canceled);
    }

    // Accept a connection that is already queued in the listen backlog
    // before the io_context ever runs. The accept can then complete on
    // the immediate path instead of parking a waiter.
    void testAcceptPendingConnection()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        // Raw blocking connect: completes via the kernel's listen
        // backlog without the io_context running. The io_context above
        // has already initialized the socket layer.
#ifdef _WIN32
        SOCKET cfd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(cfd != INVALID_SOCKET);
#else
        int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(cfd >= 0);
#endif
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int crc = ::connect(
            cfd, reinterpret_cast<sockaddr const*>(&sa), sizeof(sa));
        BOOST_TEST_EQ(crc, 0);

        tcp_socket peer(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto acceptor_task = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(peer);
            accept_ec   = aec;
            accept_done = true;
        };
        capy::run_async(ex)(acceptor_task());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(peer.is_open());
#ifdef _WIN32
        ::closesocket(cfd);
#else
        ::close(cfd);
#endif
    }

    // accept() on an open, bound, but non-listening socket fails with
    // a system error instead of hanging.
    void testAcceptWithoutListen()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        tcp_socket peer(ioc);
        std::error_code accept_ec;
        bool accept_done = false;

        auto acceptor_task = [&]() -> capy::task<> {
            auto [aec]  = co_await acc.accept(peer);
            accept_ec   = aec;
            accept_done = true;
        };
        capy::run_async(ex)(acceptor_task());

        // Watchdog: if the platform parks the accept instead of
        // failing it, retract it so the test reports the miss
        // instead of hanging the suite.
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!accept_done)
                acc.cancel();
        };
        capy::run_async(ex)(watchdog());

        ioc.run();

        BOOST_TEST(accept_done);
        // Exact errno is platform-dependent (EINVAL on Linux); only
        // require that an error is reported.
        BOOST_TEST(accept_ec);
        BOOST_TEST(!peer.is_open());
    }

    // Destroy the io_context with an accept still parked; service
    // shutdown must release the waiter without resuming it.
    void testDestroyWithParkedAccept()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        tcp_socket peer(ioc);

        auto acceptor_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };
        capy::run_async(ex)(acceptor_task());

        // Run the coroutine to its parked suspension point only, then
        // fall off the end of the scope with the accept outstanding.
        (void)ioc.run_one();
        BOOST_TEST_PASS();
    }

    void testNativeHandle()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Closed: returns the platform sentinel.
        BOOST_TEST(acc.native_handle() == invalid_native_socket);

        acc.open();
        BOOST_TEST(acc.native_handle() != invalid_native_socket);
        acc.close();
        BOOST_TEST(acc.native_handle() == invalid_native_socket);
    }

    // Drive one connect + accept + byte exchange through `acc`, which
    // must already be listening on the loopback `port`. Runs `ioc` to
    // completion; the connecting peer is spawned after the acceptor so
    // the accept is parked before the connect lands.
    bool acceptOneThrough(
        io_context& ioc, tcp_acceptor& acc, std::uint16_t port, bool v6)
    {
        tcp_socket client(ioc);
        bool done = false;

        auto server = [&]() -> capy::task<> {
            auto [aec, peer] = co_await acc.accept();
            BOOST_TEST(!aec);
            char in[8];
            auto [rec, rn] =
                co_await peer.read_some(capy::mutable_buffer(in, sizeof(in)));
            BOOST_TEST(!rec);
            done = (rn == 4);
        };
        auto sender = [&]() -> capy::task<> {
            auto [cec] = co_await client.connect(
                v6 ? endpoint(ipv6_address::loopback(), port)
                   : endpoint(ipv4_address::loopback(), port));
            BOOST_TEST(!cec);
            char const out[] = "ping";
            auto [wec, wn] =
                co_await client.write_some(capy::const_buffer(out, 4));
            BOOST_TEST(!wec);
            (void)wn;
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(server());
        capy::run_async(ex)(sender());
        ioc.run();
        return done;
    }

    // Socket activation: adopt a natively created listening descriptor
    // and accept a corosio connection through it.
    void testAssignListeningSocket()
    {
        io_context ioc(Backend);

        std::uint16_t port = 0;
        auto lfd = make_native_listener(false, port);
        BOOST_TEST(lfd != invalid_native_socket);
        BOOST_TEST(port != 0);

        tcp_acceptor acc(ioc);
        acc.assign(lfd);
        BOOST_TEST(acc.is_open());
        BOOST_TEST(acc.native_handle() == lfd);
        BOOST_TEST_EQ(acc.local_endpoint().port(), port);

        BOOST_TEST(acceptOneThrough(ioc, acc, port, false));
    }

    // A wait for readability must observe a connection that was
    // already queued when the wait began: a shared or adopted
    // listener has history the reactor never saw.
    void testWaitReadPreexistingBacklog()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        // Queue a connection before any wait exists, then pump once
        // with nothing parked so an edge-triggered reactor has
        // already dispatched — and dropped — the readiness edge.
        auto client = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(client != invalid_native_socket);
        BOOST_TEST(native_connect_loopback(client, port, false));
        (void)ioc.poll();
        ioc.restart();

        std::error_code wait_ec;
        bool wait_done      = false;
        bool watchdog_fired = false;

        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await acc.wait(wait_type::read);
            wait_ec   = wec;
            wait_done = true;
        };
        // Watchdog: a reactor that misses pre-existing readiness
        // parks forever; retract the wait so the miss is reported
        // instead of hanging the suite.
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!wait_done)
            {
                watchdog_fired = true;
                acc.cancel();
            }
        };
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(watchdog());
        ioc.run();
        ioc.restart();

        BOOST_TEST(wait_done);
        BOOST_TEST(!watchdog_fired);
        BOOST_TEST(!wait_ec);

        // The signalled connection is genuinely acceptable.
        bool accepted = false;
        auto server = [&]() -> capy::task<> {
            auto [aec, peer] = co_await acc.accept();
            BOOST_TEST(!aec);
            accepted = !aec;
        };
        capy::run_async(ex)(server());
        ioc.run();
        BOOST_TEST(accepted);

        close_native_socket(client);
    }

    // The socket-activation shape of the same guarantee: the queued
    // connection predates the adoption itself.
    void testWaitReadAdoptedBacklog()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        std::uint16_t port = 0;
        auto lfd = make_native_listener(false, port);
        BOOST_TEST(lfd != invalid_native_socket);

        auto client = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(client != invalid_native_socket);
        BOOST_TEST(native_connect_loopback(client, port, false));

        tcp_acceptor acc(ioc);
        acc.assign(lfd);
        BOOST_TEST(acc.is_open());

        // Pump once with nothing parked so the registration-time
        // readiness edge has already been dispatched and dropped.
        (void)ioc.poll();
        ioc.restart();

        std::error_code wait_ec;
        bool wait_done      = false;
        bool watchdog_fired = false;

        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await acc.wait(wait_type::read);
            wait_ec   = wec;
            wait_done = true;
        };
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!wait_done)
            {
                watchdog_fired = true;
                acc.cancel();
            }
        };
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(watchdog());
        ioc.run();
        ioc.restart();

        BOOST_TEST(wait_done);
        BOOST_TEST(!watchdog_fired);
        BOOST_TEST(!wait_ec);

        bool accepted = false;
        auto server = [&]() -> capy::task<> {
            auto [aec, peer] = co_await acc.accept();
            BOOST_TEST(!aec);
            accepted = !aec;
        };
        capy::run_async(ex)(server());
        ioc.run();
        BOOST_TEST(accepted);

        close_native_socket(client);
    }

    // Writability carries no meaning for a listener; the wait must
    // fail the same way on every backend instead of completing
    // immediately on some and never on others.
    void testWaitWriteUnsupported()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        std::error_code wait_ec;
        bool wait_done      = false;
        bool watchdog_fired = false;

        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await acc.wait(wait_type::write);
            wait_ec   = wec;
            wait_done = true;
        };
        // Watchdog: a backend that parks the meaningless wait would
        // hang the suite; retract it so the miss is reported.
        auto watchdog = [&]() -> capy::task<> {
            (void)co_await corosio::delay(std::chrono::milliseconds(250));
            if (!wait_done)
            {
                watchdog_fired = true;
                acc.cancel();
            }
        };
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(watchdog());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!watchdog_fired);
        BOOST_TEST(wait_ec == std::errc::operation_not_supported);
    }

    // Adopting over an acceptor that is already listening must retire
    // the in-flight accept machinery for the descriptor being replaced,
    // not leave it aliased onto the newly adopted one.
    void testAssignOverListeningAcceptor()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto old_port = acc.local_endpoint().port();

        // Pump once so the listen-time accept arming is live in the
        // kernel before the descriptor is swapped underneath it.
        (void)ioc.poll();
        ioc.restart();

        std::uint16_t port = 0;
        auto lfd = make_native_listener(false, port);
        BOOST_TEST(lfd != invalid_native_socket);
        BOOST_TEST(port != old_port);

        acc.assign(lfd);
        BOOST_TEST(acc.is_open());
        BOOST_TEST(acc.native_handle() == lfd);
        BOOST_TEST_EQ(acc.local_endpoint().port(), port);

        BOOST_TEST(acceptOneThrough(ioc, acc, port, false));
    }

    // release() then assign() on the SAME object: the released
    // descriptor's accept machinery must be retired before the adopted
    // one is armed.
    void testAssignAfterRelease()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        auto released = acc.release();
        BOOST_TEST(!acc.is_open());
        BOOST_TEST(released != invalid_native_socket);
        close_native_socket(released);

        std::uint16_t port = 0;
        auto lfd = make_native_listener(false, port);
        BOOST_TEST(lfd != invalid_native_socket);

        acc.assign(lfd);
        BOOST_TEST(acc.is_open());
        BOOST_TEST(acc.native_handle() == lfd);
        BOOST_TEST_EQ(acc.local_endpoint().port(), port);

        BOOST_TEST(acceptOneThrough(ioc, acc, port, false));
    }

    // release() then open()/bind()/listen() on the SAME object: the
    // shutdown state release() leaves behind must not follow the
    // acceptor onto its next descriptor, or every connection the
    // kernel hands back is dropped on arrival.
    void testListenAfterRelease()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);

        // Pump once so the listen-time accept arming is live in the
        // kernel before release() retires it.
        (void)ioc.poll();
        ioc.restart();

        auto released = acc.release();
        BOOST_TEST(!acc.is_open());
        BOOST_TEST(released != invalid_native_socket);
        close_native_socket(released);

        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST(acc.is_open());
        auto port = acc.local_endpoint().port();
        BOOST_TEST(port != 0);

        BOOST_TEST(acceptOneThrough(ioc, acc, port, false));
    }

    // Rejection matrix: bad handle, wrong type, wrong family, self.
    void testAssignRejections()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        auto expect_throw = [&](native_handle_type h) {
            bool threw = false;
            try
            {
                acc.assign(h);
            }
            catch (std::system_error const&)
            {
                threw = true;
            }
            BOOST_TEST(threw);
        };

        expect_throw(invalid_native_socket);
        BOOST_TEST(!acc.is_open());

        auto dg = make_native_socket(AF_INET, SOCK_DGRAM);
        BOOST_TEST(dg != invalid_native_socket);
        expect_throw(dg);
        BOOST_TEST(native_socket_valid(dg)); // caller keeps it
        close_native_socket(dg);

#if BOOST_COROSIO_POSIX
        auto un = make_native_socket(AF_UNIX, SOCK_STREAM);
        BOOST_TEST(un != invalid_native_socket);
        expect_throw(un);
        BOOST_TEST(native_socket_valid(un));
        close_native_socket(un);
#endif

        acc.open();
        expect_throw(acc.native_handle());
        BOOST_TEST(acc.is_open());
        acc.close();
    }

    // release() hands the listening descriptor back; it still accepts.
    void testRelease()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        auto released = acc.release();
        BOOST_TEST(!acc.is_open());
        BOOST_TEST(released != invalid_native_socket);

        auto client = make_native_socket(AF_INET, SOCK_STREAM);
        BOOST_TEST(client != invalid_native_socket);
        BOOST_TEST(native_connect_loopback(client, port, false));

        auto peer = native_accept_blocking(released);
        BOOST_TEST(peer != invalid_native_socket);

        close_native_socket(peer);
        close_native_socket(client);
        close_native_socket(released);
    }

    void testReleaseClosedThrows()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        bool caught = false;
        try
        {
            (void)acc.release();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    // Adopting a v6 listener must seed the endpoint cache as v6: the
    // IOCP accept path sizes its address buffers from that cache.
    void testAssignV6Listener()
    {
        io_context ioc(Backend);

        std::uint16_t port = 0;
        auto lfd = make_native_listener(true, port);
        if (lfd == invalid_native_socket)
            return; // no IPv6 loopback on this host

        tcp_acceptor acc(ioc);
        acc.assign(lfd);
        BOOST_TEST(acc.is_open());
        BOOST_TEST(acc.local_endpoint().is_v6());
        BOOST_TEST_EQ(acc.local_endpoint().port(), port);

        BOOST_TEST(acceptOneThrough(ioc, acc, port, true));
    }

    void run()
    {
        testConstruction();
        testListen();
        testOptions();
        testMoveConstruct();
        testMoveAssign();

        // Cancellation
        testCancelAccept();
        testCloseWhilePendingAccept();
#if !COROSIO_TEST_HAS_ASAN
        // Abandon parked coroutine frames by design; see context.hpp.
        testDestroyWithParkedAccept();
        testDestroyWithParkedRead();
#endif

        // IPv6
        testListenV6();
        testAcceptV6();
        testAcceptReturning();

        // Dual-stack
        testDualStackAccept();
        testV6OnlyAccept();

        // Fine-grained setup
        testOpenThenListen();
#ifdef SO_REUSEPORT
        testReusePort();
#endif
        testOpenIdempotent();

        // Convenience constructors
        testConvenienceConstructor();
        testConvenienceConstructorIPv6();

        // Explicit bind+listen flow
        testBindThenListen();
        testBindClosedAcceptorThrows();
        testBindAddressInUse();
        testBindError();
        testListenClosedThrows();
        testClosedAcceptorAccessors();
        testClosedAcceptorOpsThrow();

        // Waiter lifecycle
        testStopTokenAccept();
        testAcceptPendingConnection();
        testAcceptWithoutListen();

        // Descriptor adoption
        testNativeHandle();
        testAssignListeningSocket();
        testWaitReadPreexistingBacklog();
        testWaitReadAdoptedBacklog();
        testWaitWriteUnsupported();
        testAssignOverListeningAcceptor();
        testAssignAfterRelease();
        testListenAfterRelease();
        testAssignRejections();
        testRelease();
        testReleaseClosedThrows();
        testAssignV6Listener();
    }
};

COROSIO_BACKEND_TESTS(tcp_acceptor_test, "boost.corosio.acceptor")

} // namespace boost::corosio
