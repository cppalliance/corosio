//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/3.tutorials/3f.reconnect.adoc.

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

#include <boost/corosio/delay.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <stop_token>
#include <thread>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// Minimal stand-ins for the tutorial's backoff policy and retry loop;
// the page shows the full definitions, this TU only exercises the
// launch-and-cancel fragment.
struct exponential_backoff
{
};

bool delay_canceled = false;

// The delay gives the stop request something to cancel; a stopped
// token makes it complete with cond::canceled instead of waiting.
capy::task<>
connect_with_backoff(
    corosio::io_context&,
    corosio::endpoint,
    exponential_backoff,
    int)
{
    auto [ec] = co_await corosio::delay(std::chrono::seconds(2));
    delay_canceled = (ec == capy::cond::canceled);
}

struct reconnect_test
{
    void
    testStopTokenShutdown()
    {
        corosio::io_context ioc;
        corosio::endpoint ep(corosio::ipv4_address::loopback(), 8080);
        exponential_backoff backoff;
        // tag::stop_token_shutdown[]
        std::stop_source stop_src;

        capy::run_async(ioc.get_executor(), stop_src.get_token())(
            connect_with_backoff(ioc, ep, backoff, 10));

        // Later, from any thread:
        stop_src.request_stop();
        // end::stop_token_shutdown[]
        ioc.run();
        BOOST_TEST(delay_canceled);
    }

    void
    run()
    {
        testStopTokenShutdown();
    }
};

} // namespace

TEST_SUITE(reconnect_test, "boost.corosio.doc.3f_reconnect");
