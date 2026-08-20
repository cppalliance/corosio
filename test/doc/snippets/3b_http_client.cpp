//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/3.tutorials/3b.http-client.adoc.

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
#include <system_error>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

#include <iostream>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

capy::task<>
bindings_pattern(
    corosio::tcp_socket& s, corosio::endpoint ep, bool& done)
{
    // tag::error_bindings[]
    auto [ec] = co_await s.connect(ep);
    if (ec)
    {
        std::cerr << "Connect failed: " << ec.message() << "\n";
        co_return;
    }
    // end::error_bindings[]
    done = true;
}

capy::task<>
exceptions_pattern(
    corosio::tcp_socket& s, corosio::endpoint ep, bool& done)
{
    // tag::error_exceptions[]
    if (auto [ec] = co_await s.connect(ep); ec)  // Throw on error
        throw std::system_error(ec);
    // end::error_exceptions[]
    done = true;
}

capy::task<>
accept_one(corosio::tcp_acceptor& acc, corosio::tcp_socket& peer)
{
    co_await acc.accept(peer);
}

struct http_client_test
{
    // Both fragments connect for real against a loopback listener.
    template<class Fragment>
    void
    testConnectPattern(Fragment fragment)
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto ep = acc.local_endpoint();

        corosio::tcp_socket s(ioc);
        corosio::tcp_socket peer(ioc);
        BOOST_TEST(!s.open());

        bool done = false;
        capy::run_async(ex)(accept_one(acc, peer));
        capy::run_async(ex)(fragment(s, ep, done));
        ioc.run();

        BOOST_TEST(done);
        s.close();
        peer.close();
        acc.close();
        ioc.restart();
        ioc.run();
    }

    void
    run()
    {
        testConnectPattern(&bindings_pattern);
        testConnectPattern(&exceptions_pattern);
    }
};

} // namespace

TEST_SUITE(http_client_test, "boost.corosio.doc.3b_http_client");
