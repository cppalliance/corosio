//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4a.tcp-networking.adoc.

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

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <cstddef>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// Resolving contacts real DNS servers; the coroutine compiles but
// never runs.
[[maybe_unused]] capy::task<>
resolve_hostname(corosio::io_context& ioc)
{
    // tag::resolver_lookup[]
    corosio::resolver r(ioc);
    auto [ec, results] = co_await r.resolve("www.example.com", "https");

    for (auto const& entry : results)
    {
        auto ep = entry.get_endpoint();
        // Try connecting to ep...
    }
    // end::resolver_lookup[]
}

void
set_no_delay(corosio::tcp_socket& sock)
{
    // tag::no_delay[]
    sock.set_option(corosio::socket_option::no_delay(true));
    // end::no_delay[]
}

capy::task<>
read_partial_wrong(corosio::tcp_socket& sock, capy::mutable_buffer buf)
{
    // tag::read_partial[]
    // Wrong: might read less than buffer size
    auto [ec, n] = co_await sock.read_some(buf);
    // end::read_partial[]
}

capy::task<>
read_partial_right(
    corosio::tcp_socket& sock, capy::mutable_buffer buf, std::size_t& got)
{
    // tag::read_partial[]

    // Right: reads until buffer is full or EOF
    auto [ec, n] = co_await capy::read(sock, buf);
    // end::read_partial[]
    got = n;
}

struct tcp_networking_test
{
    void
    testNoDelay()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        set_no_delay(s1);
        BOOST_TEST(
            s1.get_option<corosio::socket_option::no_delay>().value());
    }

    void
    testPartialReads()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        std::size_t got = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& r, corosio::tcp_socket& w,
               std::size_t& out) -> capy::task<>
            {
                // Eight bytes ready up front: read_some may take any
                // prefix through its four-byte buffer, leaving at
                // least four for capy::read to fill completely.
                char payload[8] = {};
                co_await capy::write(
                    w, capy::const_buffer(payload, sizeof(payload)));
                char small[4];
                co_await read_partial_wrong(
                    r, capy::mutable_buffer(small, sizeof(small)));
                co_await read_partial_right(
                    r, capy::mutable_buffer(small, sizeof(small)), out);
            }(s1, s2, got));
        ioc.run();
        BOOST_TEST(got == 4u);
    }

    void
    run()
    {
        testNoDelay();
        testPartialReads();
    }
};

} // namespace

TEST_SUITE(tcp_networking_test, "boost.corosio.doc.4a_tcp_networking");
