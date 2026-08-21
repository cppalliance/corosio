//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/5.testing/5c.patterns.adoc.

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
#include <boost/corosio/test/mocket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <string>
#include <string_view>
#include <tuple>

#include "test_suite.hpp"

namespace {

// Stand-ins for the "function under test" the recipes call.
capy::task<>
my_http_get(corosio::test::mocket& m, std::string_view target)
{
    std::string req = "GET " + std::string(target) + " HTTP/1.1\r\n\r\n";
    std::ignore = co_await m.write_some(
        capy::const_buffer(req.data(), req.size()));
}

capy::task<std::string>
my_http_read(corosio::test::mocket& m)
{
    char buf[128] = {};
    auto [ec, n] = co_await m.read_some(capy::make_buffer(buf));
    if (ec)
        co_return {};
    co_return std::string(buf, n);
}

struct patterns_page_test
{
    void
    testRequestFormat()
    {
        // tag::request_format[]
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        m.expect("GET /api/v1/users HTTP/1.1\r\n\r\n");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            co_await my_http_get(m_ref, "/api/v1/users");
        };
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();

        auto ec = m.verify();  // !ec means everything was written
        m.close();
        // end::request_format[]
        BOOST_TEST(!ec);
        peer.close();
    }

    void
    testStagedResponse()
    {
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // tag::staged_response[]
        m.provide(
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "Hello");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            auto response = co_await my_http_read(m_ref);
            // assert on parsed response
        };
        // end::staged_response[]
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();
        ioc.restart();

        // A clean close proves the consumer read the whole response.
        BOOST_TEST(!m.verify());
        m.close();
        peer.close();
    }

    void
    testChunkedReads()
    {
        corosio::io_context ioc;
        // tag::chunked_reads[]
        auto [m, peer] = corosio::test::make_mocket_pair(ioc, {}, /*max_read_size=*/4);

        m.provide("ABCDEFGH");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            std::string acc;
            char buf[16];
            for (int i = 0; i < 2; ++i)
            {
                auto [ec, n] = co_await m_ref.read_some(capy::make_buffer(buf));
                acc.append(buf, n);   // n == 4 each time
            }
        };
        // end::chunked_reads[]
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();
        ioc.restart();

        // A clean close proves the loop consumed all 8 staged bytes.
        BOOST_TEST(!m.verify());
        m.close();
        peer.close();
    }

    void
    testLayering()
    {
        corosio::io_context ioc;
        // tag::layering[]
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // Pass m.socket() into a TLS stream or other layer in production code:
        corosio::tcp_socket& under = m.socket();
        // e.g., openssl_stream tls(&under, tls_ctx);
        // end::layering[]
        BOOST_TEST(under.is_open());
        BOOST_TEST(!m.verify());
        m.close();
        peer.close();
    }

    void
    testEndToEnd()
    {
        // tag::end_to_end[]
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);

        auto task = [](corosio::tcp_socket& a, corosio::tcp_socket& b)
            -> capy::task<> {
            auto [wec, wn] =
                co_await a.write_some(capy::const_buffer("payload", 7));

            char buf[16] = {};
            auto [ec, n] = co_await b.read_some(capy::make_buffer(buf));
            // buf[0..n] == "payload"
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));
        ioc.run();
        // end::end_to_end[]
        ioc.restart();

        // A reverse round trip with assertions proves data really flows.
        auto verify = [](corosio::tcp_socket& a, corosio::tcp_socket& b)
            -> capy::task<> {
            auto [wec, wn] = co_await b.write_some(
                capy::const_buffer("reply", 5));
            BOOST_TEST(!wec);

            char buf[16] = {};
            auto [ec, n] = co_await a.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(std::string_view(buf, n), "reply");
        };
        capy::run_async(ioc.get_executor())(verify(s1, s2));
        ioc.run();

        s1.close();
        s2.close();
    }

    void
    testCloseVerification()
    {
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // Deliberately leave staged data unconsumed so close() reports it.
        m.expect("never written");

        // tag::close_verification[]
        auto ec = m.verify();
        m.close();
        // ec == capy::error::test_failure means leftover provide() data was
        // never read, or expect() data was never written. Either way, the test
        // would have passed silently without this check.
        // end::close_verification[]
        BOOST_TEST(ec == capy::error::test_failure);
        peer.close();
    }

    void
    run()
    {
        testRequestFormat();
        testStagedResponse();
        testChunkedReads();
        testLayering();
        testEndToEnd();
        testCloseVerification();
    }
};

} // namespace

TEST_SUITE(patterns_page_test, "boost.corosio.doc.5c_patterns");
