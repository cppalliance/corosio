//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4f.endpoints.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::assume[]
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

namespace corosio = boost::corosio;
// end::assume[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <system_error>

#include "test_suite.hpp"

namespace capy = boost::capy;

namespace {

capy::task<>
connecting(corosio::io_context& ioc, bool& done)
{
    // tag::connecting[]
    // connect() opens the socket automatically
    corosio::tcp_socket s(ioc);

    corosio::endpoint target(
        corosio::ipv4_address::loopback(), 8080);

    auto [ec] = co_await s.connect(target);
    // end::connecting[]
    done = true;
}

// Binds the page's fixed port 8080; compiled but never executed.
[[maybe_unused]] void
accepting(corosio::io_context& ioc)
{
    // tag::accepting[]
    // Convenience constructor: open + SO_REUSEADDR + bind + listen
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));  // bind to all interfaces
    // end::accepting[]
}

// Requires external DNS; compiled but never executed.
[[maybe_unused]] capy::task<>
resolver_results_fragment(corosio::io_context& ioc)
{
    // tag::resolver_results[]
    corosio::resolver r(ioc);
    auto [ec, results] = co_await r.resolve("www.example.com", "80");

    for (auto const& entry : results)
    {
        corosio::endpoint ep = entry.get_endpoint();
        // Try connecting to ep...
    }
    // end::resolver_results[]
}

// Connects to resolved public endpoints; compiled but never executed.
[[maybe_unused]] capy::task<>
implicit_conversion(
    corosio::tcp_socket& s,
    corosio::resolver_results const& results)
{
    // tag::implicit_conversion[]
    for (corosio::endpoint ep : results)
    {
        auto [ec] = co_await s.connect(ep);
        if (!ec)
            break;
    }
    // end::implicit_conversion[]
}

struct endpoints_test
{
    void
    testOverview()
    {
        // tag::overview[]
        // IPv4 endpoint
        corosio::endpoint ep4(corosio::ipv4_address::loopback(), 8080);

        // IPv6 endpoint
        corosio::endpoint ep6(corosio::ipv6_address::loopback(), 8080);

        // Port only (binds to all interfaces)
        corosio::endpoint bind_ep(8080);
        // end::overview[]
        BOOST_TEST(ep4.is_v4());
        BOOST_TEST(ep6.is_v6());
        BOOST_TEST(bind_ep.port() == 8080);
        BOOST_TEST(bind_ep.v4_address() == corosio::ipv4_address::any());
    }

    void
    testFromV4()
    {
        // tag::from_v4[]
        auto addr = corosio::ipv4_address::loopback();  // 127.0.0.1
        corosio::endpoint ep(addr, 8080);
        // end::from_v4[]
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.port() == 8080);
    }

    void
    testFromV6()
    {
        // tag::from_v6[]
        auto addr = corosio::ipv6_address::loopback();  // ::1
        corosio::endpoint ep(addr, 8080);
        // end::from_v6[]
        BOOST_TEST(ep.is_v6());
        BOOST_TEST(ep.port() == 8080);
    }

    void
    testPortOnly()
    {
        // tag::port_only[]
        corosio::endpoint ep(8080);  // IPv4 any address (0.0.0.0)
        // end::port_only[]
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.v4_address() == corosio::ipv4_address::any());
    }

    void
    testDefaultCtor()
    {
        // tag::default_ctor[]
        corosio::endpoint ep;  // IPv4 any address, port 0
        // end::default_ctor[]
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.port() == 0);
    }

    void
    testQueryType()
    {
        corosio::endpoint ep(corosio::ipv4_address::loopback(), 8080);
        // tag::query_type[]
        if (ep.is_v4())
            std::cout << "IPv4 address\n";

        if (ep.is_v6())
            std::cout << "IPv6 address\n";
        // end::query_type[]
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(!ep.is_v6());
    }

    void
    testPort()
    {
        corosio::endpoint ep(corosio::ipv4_address::loopback(), 8080);
        // tag::port[]
        std::uint16_t port = ep.port();  // Host byte order
        // end::port[]
        BOOST_TEST(port == 8080);
    }

    void
    testV4Address()
    {
        corosio::endpoint ep(corosio::ipv4_address::loopback(), 8080);
        // tag::v4_address[]
        if (ep.is_v4())
        {
            corosio::ipv4_address addr = ep.v4_address();
            std::cout << addr.to_string() << "\n";
        }
        // end::v4_address[]
        BOOST_TEST(ep.v4_address().to_string() == "127.0.0.1");
    }

    void
    testV6Address()
    {
        corosio::endpoint ep(corosio::ipv6_address::loopback(), 8080);
        // tag::v6_address[]
        if (ep.is_v6())
        {
            corosio::ipv6_address addr = ep.v6_address();
            std::cout << addr.to_string() << "\n";
        }
        // end::v6_address[]
        BOOST_TEST(ep.v6_address().to_string() == "::1");
    }

    void
    testLoopback()
    {
        // tag::loopback[]
        // IPv4 loopback: 127.0.0.1
        auto v4_loop = corosio::ipv4_address::loopback();

        // IPv6 loopback: ::1
        auto v6_loop = corosio::ipv6_address::loopback();
        // end::loopback[]
        BOOST_TEST(v4_loop.to_string() == "127.0.0.1");
        BOOST_TEST(v6_loop.to_string() == "::1");
    }

    void
    testAny()
    {
        // tag::any[]
        // IPv4 any: 0.0.0.0 (all interfaces)
        auto v4_any = corosio::ipv4_address::any();

        // IPv6 any: :: (all interfaces)
        auto v6_any = corosio::ipv6_address::any();
        // end::any[]
        BOOST_TEST(v4_any.to_string() == "0.0.0.0");
        BOOST_TEST(v6_any.to_string() == "::");
    }

    void
    testBroadcast()
    {
        // tag::broadcast[]
        // IPv4 broadcast: 255.255.255.255
        auto v4_bcast = corosio::ipv4_address::broadcast();
        // end::broadcast[]
        BOOST_TEST(v4_bcast.to_string() == "255.255.255.255");
    }

    void
    testParseAddresses()
    {
        // tag::parse_addresses[]
        // IPv4
        corosio::ipv4_address addr;
        if (auto ec = corosio::parse_ipv4_address("192.168.1.1", addr); !ec)
        {
            corosio::endpoint ep(addr, 8080);
        }

        // IPv6
        corosio::ipv6_address addr6;
        if (auto ec = corosio::parse_ipv6_address("2001:db8::1", addr6); !ec)
        {
            corosio::endpoint ep(addr6, 8080);
        }
        // end::parse_addresses[]
        BOOST_TEST(addr.to_string() == "192.168.1.1");
        BOOST_TEST(addr6.to_string() == "2001:db8::1");
    }

    void
    testParseEndpoint()
    {
        // tag::parse_endpoint[]
        corosio::endpoint ep;
        if (auto ec = corosio::parse_endpoint("192.168.1.1:8080", ep); !ec)
        {
            // Use ep...
        }
        // end::parse_endpoint[]
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.v4_address().to_string() == "192.168.1.1");
        BOOST_TEST(ep.port() == 8080);
    }

    void
    testComparison()
    {
        // tag::comparison[]
        corosio::endpoint ep1(corosio::ipv4_address::loopback(), 8080);
        corosio::endpoint ep2(corosio::ipv4_address::loopback(), 8080);
        corosio::endpoint ep3(corosio::ipv4_address::loopback(), 9090);

        assert(ep1 == ep2);  // Same address and port
        assert(ep1 != ep3);  // Different port
        // end::comparison[]
        BOOST_TEST(ep1 == ep2);
        BOOST_TEST(ep1 != ep3);
    }

    void
    testConnecting()
    {
        // Loopback connect completes either way: refused when the port
        // is closed, connected when something happens to listen.
        corosio::io_context ioc;
        bool done = false;
        capy::run_async(ioc.get_executor())(connecting(ioc, done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    run()
    {
        testOverview();
        testFromV4();
        testFromV6();
        testPortOnly();
        testDefaultCtor();
        testQueryType();
        testPort();
        testV4Address();
        testV6Address();
        testLoopback();
        testAny();
        testBroadcast();
        testParseAddresses();
        testParseEndpoint();
        testComparison();
        testConnecting();
    }
};

} // namespace

TEST_SUITE(endpoints_test, "boost.corosio.doc.4f_endpoints");
