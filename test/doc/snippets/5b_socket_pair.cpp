//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/5.testing/5b.socket-pair.adoc.

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
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers/make_buffer.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <string_view>
#include <utility>

#include "test_suite.hpp"

namespace {

// The overview block restates the header's signature; compiling the
// redeclaration keeps the page's synopsis honest.
// tag::signature[]
template<class Socket   = corosio::tcp_socket,
         class Acceptor = corosio::tcp_acceptor,
         bool Linger    = true>
std::pair<Socket, Socket>
make_socket_pair(corosio::io_context& ctx);
// end::signature[]

struct socket_pair_page_test
{
    void
    testRoundTrip()
    {
        // tag::round_trip[]
        corosio::io_context ioc;

        auto [s1, s2] = corosio::test::make_socket_pair(ioc);

        auto task = [](corosio::tcp_socket& a, corosio::tcp_socket& b)
            -> capy::task<> {
            co_await a.write_some(capy::const_buffer("ping", 4));

            char buf[8] = {};
            auto [ec, n] = co_await b.read_some(capy::make_buffer(buf));
            // buf[0..n] == "ping"
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));
        ioc.run();
        // end::round_trip[]
        ioc.restart();

        // A reverse round trip with assertions proves data really flows.
        auto verify = [](corosio::tcp_socket& a, corosio::tcp_socket& b)
            -> capy::task<> {
            auto [wec, wn] = co_await b.write_some(
                capy::const_buffer("pong", 4));
            BOOST_TEST(!wec);
            BOOST_TEST_EQ(wn, 4u);

            char buf[8] = {};
            auto [ec, n] = co_await a.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(std::string_view(buf, n), "pong");
        };
        capy::run_async(ioc.get_executor())(verify(s1, s2));
        ioc.run();

        s1.close();
        s2.close();
    }

    void
    testLingerFalse()
    {
        corosio::io_context ioc;
        // tag::linger_false[]
        auto [s1, s2] = corosio::test::make_socket_pair<
            corosio::tcp_socket,
            corosio::tcp_acceptor,
            /*Linger=*/false>(ioc);
        // end::linger_false[]
        BOOST_TEST(s1.is_open());
        BOOST_TEST(s2.is_open());
        s1.close();
        s2.close();
    }

    void
    run()
    {
        testRoundTrip();
        testLingerFalse();
    }
};

} // namespace

TEST_SUITE(socket_pair_page_test, "boost.corosio.doc.5b_socket_pair");
