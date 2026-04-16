//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/connect.hpp>

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <system_error>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Range-based and iterator-based connect composed operation tests.
// Templated on backend so every available reactor exercises the same paths.

template<auto Backend>
struct connect_test
{
    /* Bind+listen on loopback ephemeral port; return (acceptor, port).
       Caller keeps the acceptor alive. */
    static std::uint16_t open_listener(tcp_acceptor& acc, tcp proto = tcp::v4())
    {
        acc.open(proto);
        acc.set_option(socket_option::reuse_address(true));
        std::error_code ec;
        if (proto == tcp::v6())
            ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        else
            ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        return acc.local_endpoint().port();
    }

    /* Get a port that is known to be unused right now: bind to
       port 0, read the assigned port, close. A subsequent connect to
       that port will fail with connection_refused (subject to the
       usual TOCTOU caveat, which is unavoidable for a "closed port"
       test and rarely flakes in practice). */
    static std::uint16_t pick_closed_port(io_context& ioc)
    {
        tcp_acceptor tmp(ioc);
        tmp.open();
        tmp.set_option(socket_option::reuse_address(true));
        auto ec = tmp.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = tmp.local_endpoint().port();
        tmp.close();
        return port;
    }

    void testEmptyRange()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        std::error_code result_ec{};
        endpoint result_ep;
        bool done = false;

        auto task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints;
            auto [ec, ep] = co_await corosio::connect(sock, endpoints);
            result_ec = ec;
            result_ep = ep;
            done      = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(result_ec ==
            std::make_error_code(std::errc::no_such_device_or_address));
        BOOST_TEST(result_ep == endpoint());
    }

    void testSingleGood()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto port = open_listener(acc);

        tcp_socket client(ioc);
        tcp_socket peer(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        bool connect_done = false;

        auto connect_task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), port)};
            auto [ec, ep] = co_await corosio::connect(client, endpoints);
            connect_ec   = ec;
            connected_ep = ep;
            connect_done = true;
        };

        auto accept_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };

        capy::run_async(ioc.get_executor())(accept_task());
        capy::run_async(ioc.get_executor())(connect_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST_EQ(connected_ep.port(), port);
        BOOST_TEST(connected_ep.is_v4());
        BOOST_TEST_EQ(client.remote_endpoint().port(), port);
    }

    void testBadThenGood()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto good_port = open_listener(acc);
        auto bad_port  = pick_closed_port(ioc);

        tcp_socket client(ioc);
        tcp_socket peer(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        bool connect_done = false;

        auto connect_task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), bad_port),
                endpoint(ipv4_address::loopback(), good_port)};
            auto [ec, ep] = co_await corosio::connect(client, endpoints);
            connect_ec   = ec;
            connected_ep = ep;
            connect_done = true;
        };

        auto accept_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };

        capy::run_async(ioc.get_executor())(accept_task());
        capy::run_async(ioc.get_executor())(connect_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST_EQ(connected_ep.port(), good_port);
    }

    void testAllBad()
    {
        io_context ioc(Backend);
        auto bad1 = pick_closed_port(ioc);
        auto bad2 = pick_closed_port(ioc);

        tcp_socket client(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        bool done = false;

        auto task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), bad1),
                endpoint(ipv4_address::loopback(), bad2)};
            auto [ec, ep] = co_await corosio::connect(client, endpoints);
            connect_ec   = ec;
            connected_ep = ep;
            done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(connect_ec);
        // Distinguish from the empty-range case.
        BOOST_TEST(connect_ec !=
            std::make_error_code(std::errc::no_such_device_or_address));
        BOOST_TEST(connected_ep == endpoint());
    }

    void testV4ThenV6()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto port    = open_listener(acc, tcp::v6());
        auto bad_v4  = pick_closed_port(ioc);

        tcp_socket client(ioc);
        tcp_socket peer(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        bool connect_done = false;

        auto connect_task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), bad_v4),
                endpoint(ipv6_address::loopback(), port)};
            auto [ec, ep] = co_await corosio::connect(client, endpoints);
            connect_ec   = ec;
            connected_ep = ep;
            connect_done = true;
        };

        auto accept_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };

        capy::run_async(ioc.get_executor())(accept_task());
        capy::run_async(ioc.get_executor())(connect_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST(connected_ep.is_v6());
        BOOST_TEST_EQ(connected_ep.port(), port);
    }

    void testCondSkipsAll()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto port = open_listener(acc);

        tcp_socket client(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        bool done = false;

        auto task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), port)};
            auto [ec, ep] = co_await corosio::connect(
                client,
                endpoints,
                [](std::error_code const&, endpoint const&) { return false; });
            connect_ec   = ec;
            connected_ep = ep;
            done = true;
        };

        // Must also cancel the acceptor since nothing will ever connect.
        auto cancel_task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();
            acc.cancel();
        };

        capy::run_async(ioc.get_executor())(task());
        capy::run_async(ioc.get_executor())(cancel_task());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(connect_ec ==
            std::make_error_code(std::errc::no_such_device_or_address));
        BOOST_TEST(connected_ep == endpoint());
    }

    void testCondSelectiveSkip()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto good_port = open_listener(acc);
        auto bad_port  = pick_closed_port(ioc);

        tcp_socket client(ioc);
        tcp_socket peer(ioc);

        std::error_code connect_ec{};
        endpoint connected_ep;
        int cond_calls = 0;
        bool connect_done = false;

        auto connect_task = [&]() -> capy::task<> {
            std::vector<endpoint> endpoints{
                endpoint(ipv4_address::loopback(), bad_port),
                endpoint(ipv4_address::loopback(), good_port)};
            // Skip the first (bad) endpoint, allow the second.
            auto cond = [&, good_port](
                std::error_code const&, endpoint const& e) {
                ++cond_calls;
                return e.port() == good_port;
            };
            auto [ec, ep] = co_await corosio::connect(
                client, endpoints, cond);
            connect_ec   = ec;
            connected_ep = ep;
            connect_done = true;
        };

        auto accept_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };

        capy::run_async(ioc.get_executor())(accept_task());
        capy::run_async(ioc.get_executor())(connect_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST_EQ(connected_ep.port(), good_port);
        BOOST_TEST_EQ(cond_calls, 2);
    }

    void testIteratorOverload()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);
        auto port = open_listener(acc);

        tcp_socket client(ioc);
        tcp_socket peer(ioc);

        std::vector<endpoint> endpoints{
            endpoint(ipv4_address::loopback(), port)};

        std::error_code connect_ec{};
        bool connect_done      = false;
        bool iter_matches_good = false;

        auto connect_task = [&]() -> capy::task<> {
            auto [ec, it] = co_await corosio::connect(
                client, endpoints.begin(), endpoints.end());
            connect_ec        = ec;
            iter_matches_good = (it == endpoints.begin());
            connect_done      = true;
        };

        auto accept_task = [&]() -> capy::task<> {
            (void)co_await acc.accept(peer);
        };

        capy::run_async(ioc.get_executor())(accept_task());
        capy::run_async(ioc.get_executor())(connect_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST(iter_matches_good);
    }

    void testIteratorAllFailReturnsEnd()
    {
        io_context ioc(Backend);
        auto bad = pick_closed_port(ioc);

        tcp_socket client(ioc);

        std::vector<endpoint> endpoints{
            endpoint(ipv4_address::loopback(), bad)};

        std::error_code connect_ec{};
        bool connect_done     = false;
        bool iter_is_end      = false;

        auto task = [&]() -> capy::task<> {
            auto [ec, it] = co_await corosio::connect(
                client, endpoints.begin(), endpoints.end());
            connect_ec   = ec;
            iter_is_end  = (it == endpoints.end());
            connect_done = true;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(connect_ec);
        BOOST_TEST(iter_is_end);
    }

    void testCancellation()
    {
        io_context ioc(Backend);
        tcp_socket client(ioc);

        // RFC 5737 TEST-NET-1 addresses. These are reserved for
        // documentation and will not route; connect attempts hang
        // until timeout, giving us a window to cancel.
        std::vector<endpoint> endpoints{
            endpoint(ipv4_address("192.0.2.1"), 80),
            endpoint(ipv4_address("192.0.2.2"), 80),
            endpoint(ipv4_address("192.0.2.3"), 80)};

        std::error_code connect_ec{};
        bool connect_done = false;

        auto connect_task = [&]() -> capy::task<> {
            auto [ec, ep] = co_await corosio::connect(client, endpoints);
            connect_ec    = ec;
            connect_done  = true;
            (void)ep;
        };

        auto cancel_task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();
            client.cancel();
        };

        capy::run_async(ioc.get_executor())(connect_task());
        capy::run_async(ioc.get_executor())(cancel_task());
        ioc.run();

        BOOST_TEST(connect_done);
        BOOST_TEST(connect_ec == capy::cond::canceled);
    }

    void run()
    {
        testEmptyRange();
        testSingleGood();
        testBadThenGood();
        testAllBad();
        testV4ThenV6();
        testCondSkipsAll();
        testCondSelectiveSkip();
        testIteratorOverload();
        testIteratorAllFailReturnsEnd();
        testCancellation();
    }
};

COROSIO_BACKEND_TESTS(connect_test, "boost.corosio.connect")

} // namespace boost::corosio
