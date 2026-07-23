//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4j.resolver.adoc.

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
#include <boost/corosio/resolver.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "test_suite.hpp"

namespace {

// Fragments that resolve external names or connect to external hosts
// are compiled but never executed.
[[maybe_unused]] capy::task<>
overview(corosio::io_context& ioc)
{
    // tag::overview[]
    corosio::resolver r(ioc);
    auto [ec, results] = co_await r.resolve("www.example.com", "https");

    for (auto const& entry : results)
    {
        auto ep = entry.get_endpoint();
        std::cout << ep.v4_address().to_string() << ":" << ep.port() << "\n";
    }
    // end::overview[]
}

void
construction()
{
    // tag::construction[]
    corosio::io_context ioc;
    corosio::resolver r(ioc);  // From execution context
    // end::construction[]
}

[[maybe_unused]] capy::task<>
basic_resolution(corosio::resolver& r)
{
    // tag::basic_resolution[]
    auto [ec, results] = co_await r.resolve("www.example.com", "80");
    // end::basic_resolution[]
}

[[maybe_unused]] capy::task<>
with_flags(corosio::resolver& r)
{
    // tag::with_flags[]
    auto [ec, results] = co_await r.resolve(
        "www.example.com",
        "https",
        corosio::resolve_flags::address_configured);
    // end::with_flags[]
}

// Numeric host and service perform no DNS query, so this fragment runs.
capy::task<>
combined_flags(corosio::resolver& r)
{
    // tag::combined_flags[]
    auto flags =
        corosio::resolve_flags::numeric_host |
        corosio::resolve_flags::numeric_service;

    auto [ec, results] = co_await r.resolve("127.0.0.1", "8080", flags);
    // end::combined_flags[]
    BOOST_TEST(!ec);
    BOOST_TEST(!results.empty());
    if (!results.empty())
    {
        auto ep = results.front().get_endpoint();
        BOOST_TEST(ep.is_v4());
        BOOST_TEST(ep.v4_address().to_string() == "127.0.0.1");
        BOOST_TEST(ep.port() == 8080);
    }
}

namespace results_synopsis {
using corosio::resolver_entry;
// tag::results_alias[]
using resolver_results = std::vector<resolver_entry>;
// end::results_alias[]
} // namespace results_synopsis

void
iterate_results(corosio::resolver_results const& results)
{
    // tag::iterate_results[]
    for (auto const& entry : results)
    {
        corosio::endpoint ep = entry.get_endpoint();

        if (ep.is_v4())
            std::cout << "IPv4: " << ep.v4_address().to_string();
        else
            std::cout << "IPv6: " << ep.v6_address().to_string();

        std::cout << ":" << ep.port() << "\n";
    }
    // end::iterate_results[]
}

// Abridged interface listing; declarations compile, the real class
// lives in <boost/corosio/resolver_results.hpp>.
namespace entry_synopsis {
// tag::entry_synopsis[]
class resolver_entry
{
public:
    corosio::endpoint get_endpoint() const;

    // Implicit conversion to endpoint
    operator corosio::endpoint() const;

    // Query strings used in the resolution
    std::string const& host_name() const;
    std::string const& service_name() const;
};
// end::entry_synopsis[]
} // namespace entry_synopsis

// tag::connect_to_service[]
capy::task<void> connect_to_service(
    corosio::io_context& ioc,
    std::string_view host,
    std::string_view service)
{
    corosio::resolver r(ioc);
    auto [resolve_ec, results] = co_await r.resolve(host, service);

    if (resolve_ec)
        throw std::system_error(resolve_ec);

    if (results.empty())
        throw std::runtime_error("No addresses found");

    corosio::tcp_socket sock(ioc);
    sock.open();

    std::error_code last_error;
    for (auto const& entry : results)
    {
        auto [ec] = co_await sock.connect(entry.get_endpoint());
        if (!ec)
            co_return;  // Connected successfully

        last_error = ec;
        sock.close();
        sock.open();
    }

    throw std::system_error(last_error);
}
// end::connect_to_service[]

void
cancel_pending(corosio::resolver& r)
{
    // tag::cancel[]
    r.cancel();
    // end::cancel[]
}

void
match_canceled(std::error_code ec)
{
    // tag::match_canceled[]
    if (ec == capy::cond::canceled)
    {
        // resolution was cancelled
    }
    // end::match_canceled[]
}

[[maybe_unused]] capy::task<>
sequential_resolves(corosio::resolver& resolver)
{
    // tag::single_inflight[]
    // CORRECT: Sequential resolves on same resolver
    auto [ec1, r1] = co_await resolver.resolve("host1", "80");
    auto [ec2, r2] = co_await resolver.resolve("host2", "80");

    // end::single_inflight[]
}

[[maybe_unused]] capy::task<>
parallel_resolves(corosio::io_context& ioc)
{
    // tag::single_inflight[]
    // CORRECT: Parallel resolves with separate resolver instances
    corosio::resolver r1(ioc), r2(ioc);
    // In separate coroutines:
    auto [ec1, res1] = co_await r1.resolve("host1", "80");
    auto [ec2, res2] = co_await r2.resolve("host2", "80");

    // end::single_inflight[]
}

[[maybe_unused]] void
concurrent_resolves(corosio::resolver& resolver)
{
    // tag::single_inflight[]
    // WRONG: Concurrent resolves on same resolver - UNDEFINED BEHAVIOR
    auto f1 = resolver.resolve("host1", "80");
    auto f2 = resolver.resolve("host2", "80");  // BAD: overlaps with f1
    // end::single_inflight[]
}

// tag::http_get[]
capy::task<void> http_get(
    corosio::io_context& ioc,
    std::string_view hostname)
{
    // Resolve hostname
    corosio::resolver r(ioc);
    auto [resolve_ec, results] = co_await r.resolve(hostname, "80");

    if (resolve_ec)
    {
        std::cerr << "Resolution failed: " << resolve_ec.message() << "\n";
        co_return;
    }

    // Connect to first address
    corosio::tcp_socket sock(ioc);
    sock.open();

    for (auto const& entry : results)
    {
        auto [ec] = co_await sock.connect(entry);
        if (!ec)
            break;
    }

    if (!sock.is_open())
    {
        std::cerr << "Failed to connect\n";
        co_return;
    }

    // Send HTTP request
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: " + std::string(hostname) + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (auto [ec, n] = co_await capy::write(
            sock, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    // Read the response until the server closes the connection
    std::string response;
    for (;;)
    {
        char chunk[4096];
        auto [ec, n] = co_await capy::read(
            sock, capy::mutable_buffer(chunk, sizeof(chunk)));
        response.append(chunk, n);
        if (ec)
            break;
    }

    std::cout << response << "\n";
}
// end::http_get[]

struct resolver_test
{
    void
    testConstruction()
    {
        construction();
        BOOST_TEST(true);
    }

    void
    testCombinedFlags()
    {
        corosio::io_context ioc;
        corosio::resolver r(ioc);
        capy::run_async(ioc.get_executor())(combined_flags(r));
        ioc.run();
    }

    void
    testIterateResults()
    {
        // Synthetic results keep the fragment's loop deterministic:
        // no DNS query is needed to exercise it.
        corosio::resolver_results results;
        results.emplace_back(
            corosio::endpoint(corosio::ipv4_address::loopback(), 80),
            "localhost", "http");
        results.emplace_back(
            corosio::endpoint(corosio::ipv6_address::loopback(), 443),
            "localhost", "https");
        iterate_results(results);
        BOOST_TEST(results.size() == 2);
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        corosio::resolver r(ioc);
        // Cancel with nothing in flight is a no-op.
        cancel_pending(r);
        BOOST_TEST(true);
    }

    void
    testMatchCanceled()
    {
        match_canceled(capy::error::canceled);
        match_canceled({});
        BOOST_TEST(true);
    }

    void
    testMoveSemantics()
    {
        // The page's move-semantics block is pseudocode (it shows a
        // deliberate compile error); verify the valid part here.
        corosio::io_context ioc;
        corosio::resolver r1(ioc);
        corosio::resolver r2 = std::move(r1);
        cancel_pending(r2);
        BOOST_TEST(true);
    }

    void
    run()
    {
        testConstruction();
        testCombinedFlags();
        testIterateResults();
        testCancel();
        testMatchCanceled();
        testMoveSemantics();
    }
};

} // namespace

TEST_SUITE(resolver_test, "boost.corosio.doc.4j_resolver");
