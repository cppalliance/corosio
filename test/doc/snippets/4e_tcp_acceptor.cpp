//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4e.tcp-acceptor.adoc.

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
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/capy/cond.hpp>

#include <csignal>
#include <iostream>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

#include "test_suite.hpp"

namespace {

// Fragments that bind the page's fixed port 8080 are compiled but
// never executed; the runnable tests bind ephemeral ports instead.
[[maybe_unused]] capy::task<>
overview(corosio::io_context& ioc)
{
    // tag::overview_accept[]
    // Convenience constructor: open + SO_REUSEADDR + bind + listen on port 8080
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));

    corosio::tcp_socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);

    if (!ec)
    {
        // peer is now a connected socket
    }
    // end::overview_accept[]
}

void
construction(corosio::io_context& ioc)
{
    // tag::construction[]
    // From io_context
    corosio::tcp_acceptor acc1(ioc);

    // From executor
    auto ex = ioc.get_executor();
    corosio::tcp_acceptor acc2(ex);
    // end::construction[]
}

[[maybe_unused]] void
convenience_ctor(corosio::io_context& ioc)
{
    // tag::convenience_ctor[]
    // open + SO_REUSEADDR + bind + listen; address family deduced from the endpoint
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));
    // end::convenience_ctor[]
}

[[maybe_unused]] std::error_code
bind_listen(corosio::io_context& ioc)
{
    // tag::bind_listen[]
    corosio::tcp_acceptor acc(ioc);
    if (auto ec = acc.open())                    // create an IPv4 TCP socket
        return ec;

    if (auto ec = acc.bind(corosio::endpoint(8080)))
    {
        std::cerr << "Bind failed: " << ec.message() << "\n";
        return ec;
    }

    if (auto ec = acc.listen())
    {
        std::cerr << "Listen failed: " << ec.message() << "\n";
        return ec;
    }
    // end::bind_listen[]
    return {};
}

// The page displays listen()'s declaration; compiling it inside a
// dummy struct keeps the shown signature honest.
struct listen_signature
{
    // tag::listen_signature[]
    [[nodiscard]] std::error_code listen(int backlog = 128);
    // end::listen_signature[]
};

[[maybe_unused]] void
bind_all_interfaces(corosio::io_context& ioc)
{
    // tag::bind_all_interfaces[]
    // Port only - binds to 0.0.0.0 (all IPv4 interfaces)
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));
    // end::bind_all_interfaces[]
}

[[maybe_unused]] void
bind_loopback(corosio::io_context& ioc)
{
    // tag::bind_loopback[]
    // Localhost only
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(
        corosio::ipv4_address::loopback(), 8080));
    // end::bind_loopback[]
}

capy::task<>
accept_one(
    corosio::io_context& ioc,
    corosio::tcp_acceptor& acc,
    std::error_code& out)
{
    // tag::accept_into_peer[]
    corosio::tcp_socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);
    // end::accept_into_peer[]
    out = ec;
}

capy::task<>
accept_value(corosio::tcp_acceptor& acc, std::error_code& out)
{
    // tag::accept_returning[]
    auto [ec, peer] = co_await acc.accept();
    // end::accept_returning[]
    out = ec;
}

void
cancel_pending(corosio::tcp_acceptor& acc)
{
    // tag::cancel[]
    acc.cancel();
    // end::cancel[]
}

capy::task<>
stop_token_accept(
    corosio::tcp_acceptor& acc,
    corosio::tcp_socket& peer,
    bool& canceled)
{
    // tag::accept_stop_token[]
    // Inside a cancellable task:
    auto [ec] = co_await acc.accept(peer);
    if (ec == capy::cond::canceled)
        std::cout << "Accept cancelled\n";
    // end::accept_stop_token[]
    canceled = ec == capy::cond::canceled;
}

void
close_acceptor(corosio::tcp_acceptor& acc)
{
    // tag::close[]
    acc.close();
    // end::close[]
}

bool
ready_to_accept(corosio::tcp_acceptor& acc)
{
    // tag::is_open[]
    if (acc.is_open())
    {
        // Ready to accept
    }
    // end::is_open[]
    return acc.is_open();
}

// The deleted-copy line renders from the excluded region below; the
// static_assert keeps its claim true against the real class.
static_assert(!std::is_copy_constructible_v<corosio::tcp_acceptor>);

void
move_semantics(corosio::io_context& ioc)
{
    // tag::move_only[]
    corosio::tcp_acceptor acc1(ioc);
    corosio::tcp_acceptor acc2 = std::move(acc1);  // OK
    // end::move_only[]
#if 0
    // tag::move_only[]

    corosio::tcp_acceptor acc3 = acc2;  // Error: deleted copy constructor
    // end::move_only[]
#endif

    // tag::move_assign[]
    acc1 = std::move(acc2);  // Closes acc1's socket if open, then moves acc2
    // end::move_assign[]
}

capy::task<>
handle_connection(corosio::tcp_socket peer)
{
    co_return;
}

// tag::accept_loop[]
capy::task<void> accept_loop(
    corosio::io_context& ioc,
    corosio::tcp_acceptor& acc)
{
    for (;;)
    {
        corosio::tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);

        if (ec)
        {
            if (ec == capy::cond::canceled)
                break;  // Shutdown requested

            std::cerr << "Accept error: " << ec.message() << "\n";
            continue;  // Try again
        }

        // Spawn a coroutine to handle this connection
        capy::run_async(ioc.get_executor())(
            handle_connection(std::move(peer)));
    }
}
// end::accept_loop[]

// run_server waits on real process signals; compiling it is the test.
// tag::graceful_shutdown[]
capy::task<void> run_server(corosio::io_context& ioc)
{
    corosio::tcp_acceptor acc(ioc);
    if (auto ec = acc.open())
        co_return;
    if (auto ec = acc.bind(corosio::endpoint(8080)))
    {
        std::cerr << "Bind failed: " << ec.message() << "\n";
        co_return;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "Listen failed: " << ec.message() << "\n";
        co_return;
    }

    corosio::signal_set signals(ioc, SIGINT, SIGTERM);

    // Spawn accept loop
    capy::run_async(ioc.get_executor())(accept_loop(ioc, acc));

    // Wait for shutdown signal
    auto [ec, signum] = co_await signals.wait();
    if (!ec)
    {
        std::cout << "Received signal " << signum << ", shutting down\n";
        acc.cancel();  // Stop accepting
        // Existing connections continue until complete
    }
}
// end::graceful_shutdown[]

capy::task<>
connect_client(
    corosio::io_context& ioc,
    corosio::endpoint ep,
    std::error_code& out)
{
    corosio::tcp_socket s(ioc);
    BOOST_TEST(!s.open());
    auto [ec] = co_await s.connect(ep);
    out = ec;
}

struct tcp_acceptor_test
{
    void
    testConstruction()
    {
        corosio::io_context ioc;
        construction(ioc);
        move_semantics(ioc);
    }

    void
    testAccept()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(corosio::endpoint(
            corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        BOOST_TEST(ready_to_accept(acc));
        auto ep = acc.local_endpoint();

        std::error_code accept_ec;
        std::error_code connect_ec;
        capy::run_async(ex)(accept_one(ioc, acc, accept_ec));
        capy::run_async(ex)(connect_client(ioc, ep, connect_ec));
        ioc.run();
        BOOST_TEST(!accept_ec);
        BOOST_TEST(!connect_ec);

        ioc.restart();
        std::error_code accept2_ec;
        std::error_code connect2_ec;
        capy::run_async(ex)(accept_value(acc, accept2_ec));
        capy::run_async(ex)(connect_client(ioc, ep, connect2_ec));
        ioc.run();
        BOOST_TEST(!accept2_ec);
        BOOST_TEST(!connect2_ec);

        close_acceptor(acc);
        BOOST_TEST(!ready_to_accept(acc));
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(corosio::endpoint(
            corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::error_code accept_ec;
        capy::run_async(ex)(accept_one(ioc, acc, accept_ec));
        // FIFO posting: the accept is already pending when cancel runs
        capy::run_async(ex)(
            [](corosio::tcp_acceptor& a) -> capy::task<> {
                cancel_pending(a);
                co_return;
            }(acc));
        ioc.run();
        BOOST_TEST(accept_ec == capy::cond::canceled);
    }

    void
    testStopToken()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(corosio::endpoint(
            corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        corosio::tcp_socket peer(ioc);
        std::stop_source source;
        bool canceled = false;
        capy::run_async(ex, source.get_token())(
            stop_token_accept(acc, peer, canceled));
        capy::run_async(ex)(
            [](std::stop_source& s) -> capy::task<> {
                s.request_stop();
                co_return;
            }(source));
        ioc.run();
        BOOST_TEST(canceled);
    }

    void
    run()
    {
        testConstruction();
        testAccept();
        testCancel();
        testStopToken();
    }
};

} // namespace

TEST_SUITE(tcp_acceptor_test, "boost.corosio.doc.4e_tcp_acceptor");
