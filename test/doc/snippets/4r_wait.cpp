//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4r.wait.adoc.

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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timeout.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
using namespace std::chrono_literals;
// end::assume[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>

#if BOOST_COROSIO_POSIX
#include <unistd.h>
#endif

#include <system_error>
#include <utility>

#include "test_suite.hpp"

namespace {

// The page shows the enum's shape; the real one lives in
// <boost/corosio/wait_type.hpp>.
namespace api_sketch {
// tag::wait_type_enum[]
enum class wait_type { read, write, error };
// end::wait_type_enum[]
} // namespace api_sketch

capy::task<>
wait_readable(corosio::tcp_socket& sock, std::error_code& ec_out)
{
    // tag::wait_read[]
    auto [ec] = co_await sock.wait(corosio::wait_type::read);
    if (!ec) {
        // sock is readable: a subsequent read_some will return data
        // without blocking.
    }
    // end::wait_read[]
    ec_out = ec;
}

#if BOOST_COROSIO_POSIX
// Stand-ins for a C library that owns a nonblocking socket and does
// its own I/O on it (the libpq shape).
struct foreign_conn
{
};
int foreign_socket(foreign_conn*) { return -1; }
bool foreign_wants_read(foreign_conn*) { return false; }
int foreign_consume(foreign_conn*) { return 0; }
int foreign_flush(foreign_conn*) { return 0; }

capy::task<std::error_code>
drive_foreign(corosio::io_context& ioc, foreign_conn* conn)
{
    // tag::foreign_adopt[]
    // Adopt a duplicate: assigned means owned, and corosio closing
    // the duplicate can never close the library's descriptor.
    // Readiness travels through the shared open file description.
    corosio::tcp_socket sock(ioc);
    if (auto ec = sock.assign(::dup(foreign_socket(conn))))
        co_return ec;

    // Read side: wake, then let the library take the bytes itself.
    while (foreign_wants_read(conn)) {
        auto [ec] = co_await sock.wait(corosio::wait_type::read);
        if (ec) co_return ec;
        if (foreign_consume(conn) != 0)
            co_return std::make_error_code(std::errc::io_error);
    }

    // Write side: retry exactly when the socket can make progress.
    while (foreign_flush(conn) == 1) {
        auto [ec] = co_await sock.wait(corosio::wait_type::write);
        if (ec) co_return ec;
    }
    // end::foreign_adopt[]
    co_return std::error_code{};
}
#endif

capy::task<>
wait_then_accept(
    corosio::io_context& ioc, corosio::tcp_acceptor& acceptor,
    std::error_code& wec_out, std::error_code& aec_out)
{
    // tag::acceptor_wait[]
    auto [wec] = co_await acceptor.wait(corosio::wait_type::read);
    if (wec) co_return;

    corosio::tcp_socket peer(ioc);
    auto [aec] = co_await acceptor.accept(peer);
    // end::acceptor_wait[]
    wec_out = wec;
    aec_out = aec;
}

capy::task<>
wait_with_deadline(corosio::tcp_socket& sock, std::error_code& ec_out)
{
    // tag::wait_timeout[]
    auto [ec] = co_await corosio::timeout(
        sock.wait(corosio::wait_type::read), 200ms);
    if (ec == capy::cond::timeout)
        std::cout << "No readiness within 200ms\n";
    // end::wait_timeout[]
    ec_out = ec;
}

capy::task<>
await_and_flag(capy::task<> t, bool& done)
{
    co_await std::move(t);
    done = true;
}

struct wait_test
{
    void
    testWaitRead()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        auto [sock, peer] = boost::corosio::test::make_socket_pair(ioc);

        std::error_code ec = std::make_error_code(std::errc::io_error);
        capy::run_async(ex)(wait_readable(sock, ec));

        auto writer = [&]() -> capy::task<>
        {
            co_await peer.write_some(capy::const_buffer("x", 1));
        };
        capy::run_async(ex)(writer());

        ioc.run();
        BOOST_TEST(!ec);
    }

    void
    testAcceptorWait()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        corosio::tcp_acceptor acceptor(ioc);
        BOOST_TEST(!acceptor.open());
        acceptor.set_option(corosio::socket_option::reuse_address(true));
        auto bec = acceptor.bind(corosio::endpoint(
            corosio::ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        BOOST_TEST(!acceptor.listen());
        auto port = acceptor.local_endpoint().port();

        // Sentinels distinguish "never reached" from success.
        std::error_code wec = std::make_error_code(std::errc::io_error);
        std::error_code aec = std::make_error_code(std::errc::io_error);
        capy::run_async(ex)(wait_then_accept(ioc, acceptor, wec, aec));

        corosio::tcp_socket client(ioc);
        BOOST_TEST(!client.open());
        auto connecter = [&]() -> capy::task<>
        {
            co_await client.connect(corosio::endpoint(
                corosio::ipv4_address::loopback(), port));
        };
        capy::run_async(ex)(connecter());

        ioc.run();
        BOOST_TEST(!wec);
        BOOST_TEST(!aec);
    }

    void
    testWaitCancel()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        auto [sock, peer] = boost::corosio::test::make_socket_pair(ioc);

        // tag::wait_cancel[]
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(corosio::wait_type::read);
            // ec == capy::cond::canceled if sock.cancel() was invoked
        };
        // end::wait_cancel[]

        bool done = false;
        capy::run_async(ex)(await_and_flag(waiter(), done));

        // Posted after the waiter, so the wait is in flight when the
        // cancel lands.
        auto canceller = [&]() -> capy::task<>
        {
            sock.cancel();
            co_return;
        };
        capy::run_async(ex)(canceller());

        ioc.run();
        BOOST_TEST(done);
    }

    void
    testWaitTimeout()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        // The peer stays silent, so readiness never arrives.
        auto [sock, peer] = boost::corosio::test::make_socket_pair(ioc);

        std::error_code ec;
        capy::run_async(ex)(wait_with_deadline(sock, ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::timeout);
    }

    void
    run()
    {
        testWaitRead();
        testAcceptorWait();
        testWaitCancel();
        testWaitTimeout();
    }
};

} // namespace

TEST_SUITE(wait_test, "boost.corosio.doc.4r_wait");
