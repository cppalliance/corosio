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
#include <boost/corosio/timer.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#ifndef _WIN32
// For the SO_REUSEPORT guard around testReusePort. The corosio public
// option header is platform-agnostic and does not expose this macro.
#include <sys/socket.h>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

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
            // Start a timer to cancel the accept
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

            // Launch accept that will block (no incoming connections)
            // Store lambda in variable to ensure it outlives the coroutine.
            auto nested_coro = [&acc, &peer, &accept_done,
                                &accept_ec]() -> capy::task<> {
                auto [ec]   = co_await acc.accept(peer);
                accept_ec   = ec;
                accept_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait for timer then cancel
            (void)co_await t.wait();
            acc.cancel();

            // Wait for accept to complete
            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(accept_done);
            BOOST_TEST(accept_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        acc.close();
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
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));

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
            (void)co_await t.wait();
            acc.close();

            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

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
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(200));
            (void)co_await t.wait();
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

    void run()
    {
        testConstruction();
        testListen();
        testMoveConstruct();
        testMoveAssign();

        // Cancellation
        testCancelAccept();
        testCloseWhilePendingAccept();

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
    }
};

COROSIO_BACKEND_TESTS(tcp_acceptor_test, "boost.corosio.acceptor")

} // namespace boost::corosio
