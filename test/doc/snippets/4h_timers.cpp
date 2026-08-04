//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4h.timers.adoc.

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
#include <boost/corosio/delay.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/capy/cond.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
using namespace std::chrono_literals;
// end::assume[]

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <iostream>
#include <stop_token>
#include <system_error>

#include "test_suite.hpp"

namespace {

capy::task<> delay_duration_frag(std::error_code& out)
{
    // tag::delay_duration[]
    auto [ec] = co_await corosio::delay(500ms);
    if (!ec)
        std::cout << "500ms elapsed\n";
    // end::delay_duration[]
    out = ec;
}

capy::task<> delay_timepoint_frag()
{
    // tag::delay_timepoint[]
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i)
    {
        next += 100ms;
        auto [ec] = co_await corosio::delay(next);
        if (ec)
            break;
        std::cout << "Tick " << i << "\n";
    }
    // end::delay_timepoint[]
}

// This fragment waits on a real 5-minute wall-clock deadline, so
// (like the connect fragments below) it is compiled but never
// launched.
capy::task<> delay_wallclock_frag()
{
    // tag::delay_wallclock[]
    auto deadline = std::chrono::system_clock::now() +
        std::chrono::minutes(5);
    auto [ec] = co_await corosio::delay(deadline);
    // end::delay_wallclock[]
}

// tag::delay_traits[]
// Re-read the wall clock at least once per second, so a step of
// the clock is observed within that bound
struct capped_traits
{
    static std::chrono::system_clock::duration
    to_wait_duration(std::chrono::system_clock::duration d)
    {
        auto cap = std::chrono::system_clock::duration(
            std::chrono::seconds(1));
        return d < cap ? d : cap;
    }
};
// end::delay_traits[]

// Same real deadline as the wallclock fragment above: compiled but
// never launched.
capy::task<> delay_traits_frag()
{
    // tag::delay_traits_use[]
    auto deadline = std::chrono::system_clock::now() +
        std::chrono::minutes(5);
    auto [ec] = co_await corosio::delay<capped_traits>(deadline);
    // end::delay_traits_use[]
}

capy::task<> delay_cancel_frag(std::error_code& out)
{
    // tag::delay_cancel[]
    auto [ec] = co_await corosio::delay(10s);
    if (ec == capy::cond::canceled)
        std::cout << "Delay was cancelled\n";
    // end::delay_cancel[]
    out = ec;
}

capy::task<> timeout_read_frag(
    corosio::tcp_socket& sock, capy::mutable_buffer buffer,
    std::error_code& out)
{
    // tag::timeout_read[]
    auto [ec, n] = co_await corosio::timeout(
        sock.read_some(buffer), 200ms);

    if (ec == capy::cond::timeout)
        std::cout << "No data within 200ms\n";
    else if (!ec)
        std::cout << "Read " << n << " bytes\n";
    // end::timeout_read[]
    out = ec;
}

// The connect fragments dial real endpoints, so the coroutines are
// compiled but never launched.
capy::task<> timeout_deadline_frag(
    corosio::tcp_socket& sock, corosio::endpoint ep)
{
    // tag::timeout_deadline[]
    auto deadline = std::chrono::steady_clock::now() + 5s;
    auto [ec] = co_await corosio::timeout(sock.connect(ep), deadline);
    // end::timeout_deadline[]
}

capy::task<> timeout_vs_cancel_frag(
    corosio::tcp_socket& sock, corosio::endpoint ep)
{
    // tag::timeout_vs_cancel[]
    auto [ec] = co_await corosio::timeout(sock.connect(ep), 3s);
    if (ec == capy::cond::timeout)
        std::cout << "Connect attempt timed out\n";
    else if (ec == capy::cond::canceled)
        std::cout << "Connect attempt cancelled by caller\n";
    // end::timeout_vs_cancel[]
}

// tag::connect_retry[]
capy::task<>
connect_with_deadline(
    corosio::tcp_socket& sock,
    corosio::endpoint ep,
    int max_attempts)
{
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        auto [ec] = co_await corosio::timeout(sock.connect(ep), 3s);
        if (!ec)
            co_return;

        if (ec == capy::cond::canceled)
            co_return;

        sock.close();
        auto [dec] = co_await corosio::delay(500ms);
        if (dec == capy::cond::canceled)
            co_return;
    }
}
// end::connect_retry[]

struct timers_test
{
    void
    testDelayDuration()
    {
        corosio::io_context ioc;
        std::error_code ec = make_error_code(std::errc::io_error);
        capy::run_async(ioc.get_executor())(delay_duration_frag(ec));
        ioc.run();
        BOOST_TEST(!ec);
    }

    void
    testDelayTimePoint()
    {
        corosio::io_context ioc;
        auto start = std::chrono::steady_clock::now();
        capy::run_async(ioc.get_executor())(delay_timepoint_frag());
        ioc.run();
        auto elapsed = std::chrono::steady_clock::now() - start;
        BOOST_TEST(elapsed >= 1s);
    }

    void
    testDelayCancel()
    {
        corosio::io_context ioc;
        std::error_code ec;
        std::stop_source source;
        source.request_stop();
        capy::run_async(ioc.get_executor(), source.get_token())(
            delay_cancel_frag(ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    void
    testTimeoutRead()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        char data[128];
        std::error_code ec;
        capy::run_async(ioc.get_executor())(timeout_read_frag(
            s1, capy::mutable_buffer(data, sizeof(data)), ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::timeout);
    }

    void
    run()
    {
        testDelayDuration();
        testDelayTimePoint();
        testDelayCancel();
        testTimeoutRead();
    }
};

} // namespace

TEST_SUITE(timers_test, "boost.corosio.doc.4h_timers");
