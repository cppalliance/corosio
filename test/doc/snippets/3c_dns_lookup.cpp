//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/3.tutorials/3c.dns-lookup.adoc.

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

#include <boost/corosio.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// Resolving a public hostname needs the network; compiled, never run.
[[maybe_unused]] capy::task<>
resolver_overview(corosio::io_context& ioc)
{
    // tag::resolver_overview[]
    corosio::resolver r(ioc);
    auto [ec, results] = co_await r.resolve("www.example.com", "https");
    // end::resolver_overview[]
}

std::uint16_t
inspect_endpoint(corosio::resolver_entry const& entry)
{
    // tag::entry_endpoint[]
    auto ep = entry.get_endpoint();

    if (ep.is_v4())
    {
        // IPv4 address
        corosio::ipv4_address addr = ep.v4_address();
    }
    else
    {
        // IPv6 address
        corosio::ipv6_address addr = ep.v6_address();
    }

    std::uint16_t port = ep.port();
    // end::entry_endpoint[]
    return port;
}

// Numeric flags skip DNS entirely, so this fragment runs for real.
capy::task<>
resolve_numeric(
    corosio::io_context& ioc,
    std::error_code& out_ec,
    std::size_t& out_count)
{
    corosio::resolver r(ioc);
    std::string_view host = "127.0.0.1";
    std::string_view service = "8080";
    // tag::resolve_with_flags[]
    auto [ec, results] = co_await r.resolve(
        host, service,
        corosio::resolve_flags::numeric_host |
        corosio::resolve_flags::numeric_service);
    // end::resolve_with_flags[]
    out_ec = ec;
    out_count = results.size();
}

// Connecting to a resolved host needs the network; compiled, never run.
// tag::connect_to_host[]
capy::task<void> connect_to_host(
    corosio::io_context& ioc,
    std::string_view host,
    std::string_view service)
{
    corosio::resolver r(ioc);
    auto [resolve_ec, results] = co_await r.resolve(host, service);
    if (resolve_ec)
        throw std::system_error(resolve_ec);

    corosio::tcp_socket sock(ioc);

    // Try each address until one works; connect() opens the socket
    std::error_code last_ec;
    for (auto const& entry : results)
    {
        auto [ec] = co_await sock.connect(entry.get_endpoint());
        if (!ec)
        {
            std::cout << "Connected to " << host << "\n";
            co_return;
        }
        last_ec = ec;
    }

    throw std::system_error(last_ec, "all addresses failed");
}
// end::connect_to_host[]

[[maybe_unused]] capy::task<void> (* const connect_demo)(
    corosio::io_context&, std::string_view, std::string_view) =
        &connect_to_host;

struct dns_lookup_test
{
    void
    testEntryEndpoint()
    {
        corosio::resolver_entry entry(
            corosio::endpoint(corosio::ipv4_address::loopback(), 443),
            "example.com", "https");
        BOOST_TEST(inspect_endpoint(entry) == 443);
    }

    void
    testResolveWithFlags()
    {
        corosio::io_context ioc;
        std::error_code ec;
        std::size_t count = 0;
        capy::run_async(ioc.get_executor())(
            resolve_numeric(ioc, ec, count));
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST(count > 0);
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        corosio::resolver r(ioc);
        // tag::resolver_cancel[]
        r.cancel();  // Cancel pending operation
        // end::resolver_cancel[]
        BOOST_TEST(true);
    }

    void
    run()
    {
        testEntryEndpoint();
        testResolveWithFlags();
        testCancel();
    }
};

} // namespace

TEST_SUITE(dns_lookup_test, "boost.corosio.doc.3c_dns_lookup");
