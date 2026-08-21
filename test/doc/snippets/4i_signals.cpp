//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4i.signals.adoc.

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
#include <boost/corosio/signal_set.hpp>
#include <csignal>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <iostream>
#include <system_error>
#include <tuple>
#include <type_traits>

#include "test_suite.hpp"

namespace {

// The page's move-semantics block is shown as pseudocode because its
// last line is intentionally ill-formed; these assertions keep the
// claims honest.
static_assert(std::is_move_constructible_v<corosio::signal_set>);
static_assert(!std::is_copy_constructible_v<corosio::signal_set>);

capy::task<> overview_frag(corosio::io_context& ioc, int& out)
{
    // tag::overview[]
    corosio::signal_set signals(ioc, SIGINT, SIGTERM);

    auto [ec, signum] = co_await signals.wait();
    if (!ec)
        std::cout << "Received signal " << signum << "\n";
    // end::overview[]
    BOOST_TEST(!ec);
    out = signum;
}

capy::task<> raise_signal(int signum)
{
    std::raise(signum);
    co_return;
}

capy::task<> wait_switch_frag(corosio::signal_set& signals)
{
    // tag::wait_switch[]
    auto [ec, signum] = co_await signals.wait();

    if (!ec)
    {
        switch (signum)
        {
        case SIGINT:
            std::cout << "Interrupt received\n";
            break;
        case SIGTERM:
            std::cout << "Termination requested\n";
            break;
        }
    }
    // end::wait_switch[]
    BOOST_TEST(!ec);
    BOOST_TEST_EQ(signum, SIGINT);
}

capy::task<> cancel_result_frag(
    corosio::signal_set& signals, std::error_code& out)
{
    // tag::cancel_result[]
    auto [ec, signum] = co_await signals.wait();
    if (ec == capy::cond::canceled)
        std::cout << "Wait was cancelled\n";
    // end::cancel_result[]
    out = ec;
}

// The use-case coroutines wait for operator-sent signals in loops, so
// they are compiled but never launched. Fragments naming POSIX-only
// signals or sigaction flags follow the unit tests' platform guard.

// tag::graceful_shutdown[]
capy::task<void> shutdown_handler(
    corosio::io_context& ioc,
    std::atomic<bool>& running)
{
    corosio::signal_set signals(ioc, SIGINT, SIGTERM);

    auto [ec, signum] = co_await signals.wait();
    if (!ec)
    {
        std::cout << "Shutdown signal received\n";
        running = false;
        ioc.stop();
    }
}
// end::graceful_shutdown[]

#if BOOST_COROSIO_POSIX

// tag::signal_loop[]
capy::task<void> signal_loop(corosio::io_context& ioc)
{
    corosio::signal_set signals(ioc, SIGUSR1);

    for (;;)
    {
        auto [ec, signum] = co_await signals.wait();
        if (ec)
            break;

        std::cout << "Received USR1, doing work...\n";
        // Handle signal
    }
}
// end::signal_loop[]

struct Config
{
    void reload() {}
};

// tag::config_reload[]
capy::task<void> config_reloader(
    corosio::io_context& ioc,
    Config& config)
{
    corosio::signal_set signals(ioc, SIGHUP);

    for (;;)
    {
        auto [ec, signum] = co_await signals.wait();
        if (ec)
            break;

        std::cout << "Reloading configuration...\n";
        config.reload();
    }
}
// end::config_reload[]

// tag::child_reaper[]
capy::task<void> child_reaper(corosio::io_context& ioc)
{
    using flags = corosio::signal_set;

    corosio::signal_set signals(ioc);

    // Only notify on child termination, not stop/continue
    // Prevent zombie processes automatically
    if (signals.add(SIGCHLD, flags::no_child_stop | flags::no_child_wait))
        co_return;

    for (;;)
    {
        auto [ec, signum] = co_await signals.wait();
        if (ec)
            break;

        // With no_child_wait, children are reaped automatically
        std::cout << "Child process terminated\n";
    }
}
// end::child_reaper[]

#endif // BOOST_COROSIO_POSIX

// tag::server_shutdown[]
capy::task<void> run_server(corosio::io_context& ioc)
{
    std::atomic<bool> running{true};

    // Start signal handler
    capy::run_async(ioc.get_executor())(
        [](corosio::io_context& ioc, std::atomic<bool>& running)
            -> capy::task<void>
        {
            corosio::signal_set signals(ioc, SIGINT, SIGTERM);
            co_await signals.wait();
            running = false;
            ioc.stop();
        }(ioc, running));

    // Accept loop
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));

    while (running)
    {
        corosio::tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec)
            break;

        // Handle connection...
    }
}
// end::server_shutdown[]

struct signals_test
{
    void
    testOverview()
    {
        corosio::io_context ioc;
        int received = 0;
        capy::run_async(ioc.get_executor())(overview_frag(ioc, received));
        capy::run_async(ioc.get_executor())(raise_signal(SIGTERM));
        ioc.run();
        BOOST_TEST_EQ(received, SIGTERM);
    }

    void
    testConstructEmpty()
    {
        corosio::io_context ioc;
        // tag::construct_empty[]
        corosio::signal_set signals(ioc);
        std::error_code ec = signals.add(SIGINT);
        if (! ec)
            ec = signals.add(SIGTERM);
        // end::construct_empty[]
        BOOST_TEST(!ec);
    }

#if BOOST_COROSIO_POSIX
    void
    testConstructInitial()
    {
        corosio::io_context ioc;
        // tag::construct_initial[]
        // One signal
        corosio::signal_set s1(ioc, SIGINT);

        // Two signals
        corosio::signal_set s2(ioc, SIGINT, SIGTERM);

        // Three signals
        corosio::signal_set s3(ioc, SIGINT, SIGTERM, SIGHUP);
        // end::construct_initial[]
    }

    void
    testAdd()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc);
        // tag::add_signal[]
        if (auto ec = signals.add(SIGUSR1))
            std::cout << "add failed: " << ec.message() << "\n";
        // end::add_signal[]
    }

    void
    testAddFlags()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc);
        // tag::add_flags[]
        using flags = corosio::signal_set;

        // Restart interrupted system calls automatically
        std::error_code ec = signals.add(SIGHUP, flags::restart);

        // Multiple flags can be combined
        if (! ec)
            ec = signals.add(
                SIGCHLD, flags::restart | flags::no_child_stop);
        // end::add_flags[]
        BOOST_TEST(!ec);
    }

    void
    testFlagCompat()
    {
        corosio::io_context ioc;
        using flags = corosio::signal_set;
        // tag::flag_compat[]
        corosio::signal_set s1(ioc);
        corosio::signal_set s2(ioc);

        std::error_code ec;
        ec = s1.add(SIGINT, flags::restart);   // OK - first registration
        ec = s2.add(SIGINT, flags::restart);   // OK - same flags
        ec = s2.add(SIGINT, flags::no_defer);  // invalid_argument - different flags

        // Use dont_care to accept existing flags
        ec = s2.add(SIGINT, flags::dont_care); // OK - accepts existing flags
        // end::flag_compat[]
        BOOST_TEST(!ec);
        BOOST_TEST(
            s2.add(SIGINT, flags::no_defer) == std::errc::invalid_argument);
    }
#endif // BOOST_COROSIO_POSIX

    void
    testRemove()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc, SIGINT);
        // tag::remove_signal[]
        std::error_code ec = signals.remove(SIGINT);

        // Removing a signal that's not in the set is not an error
        ec = signals.remove(SIGINT);
        // end::remove_signal[]
        BOOST_TEST(!ec);
    }

    void
    testClear()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc, SIGINT, SIGTERM);
        // tag::clear_signals[]
        std::error_code ec = signals.clear();
        // end::clear_signals[]
        BOOST_TEST(!ec);
    }

    void
    testWaitSwitch()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc, SIGINT, SIGTERM);
        capy::run_async(ioc.get_executor())(wait_switch_frag(signals));
        capy::run_async(ioc.get_executor())(raise_signal(SIGINT));
        ioc.run();
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        corosio::signal_set signals(ioc, SIGINT);
        std::error_code ec;
        capy::run_async(ioc.get_executor())(
            cancel_result_frag(signals, ec));
        capy::run_async(ioc.get_executor())(
            [](corosio::signal_set& signals) -> capy::task<>
            {
                // tag::cancel_call[]
                signals.cancel();
                // end::cancel_call[]
                co_return;
            }(signals));
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    void
    run()
    {
        testOverview();
        testConstructEmpty();
#if BOOST_COROSIO_POSIX
        testConstructInitial();
        testAdd();
        testAddFlags();
        testFlagCompat();
#endif
        testRemove();
        testClear();
        testWaitSwitch();
        testCancel();
    }
};

} // namespace

TEST_SUITE(signals_test, "boost.corosio.doc.4i_signals");
