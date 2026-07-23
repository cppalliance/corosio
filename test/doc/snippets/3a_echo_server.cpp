//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/3.tutorials/3a.echo-server.adoc.

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

#include <boost/corosio/tcp_server.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>

#include <cstring>
#include <string>
#include <system_error>

#include "test_suite.hpp"

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// Two same-name regions concatenate into the page's write_some/write
// comparison; separate scopes keep the duplicate bindings legal.
capy::task<>
composed_write_demo(
    corosio::tcp_socket& sock, capy::const_buffer buf, bool& done)
{
    {
        // tag::composed_write[]
        // write_some: may write partial data
        auto [ec, n] = co_await sock.write_some(buf);  // n might be < buf.size()

        // end::composed_write[]
        BOOST_TEST(!ec);
    }
    {
        // tag::composed_write[]
        // write: writes all data or fails
        auto [ec, n] = co_await capy::write(sock, buf);  // n == buf.size() or error
        // end::composed_write[]
        BOOST_TEST(!ec);
        BOOST_TEST(n == buf.size());
    }
    done = true;
}

capy::task<>
advance_then_check_session(
    corosio::tcp_socket& sock, capy::mutable_buffer buf, bool& done)
{
    for (;;)
    {
        // tag::advance_then_check[]
        auto [ec, n] = co_await sock.read_some(buf);
        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(buf.data(), n));
        if (wec || ec)
            break;  // Normal termination path
        // end::advance_then_check[]
    }
    done = true;
}

capy::task<>
exception_style_session(
    corosio::tcp_socket& sock, capy::mutable_buffer buf, bool& done)
{
    // tag::exceptions_eof[]
    try {
        auto [ec, n] = co_await sock.read_some(buf);
        if (ec) throw std::system_error(ec);
    } catch (...) {
        // EOF is an exception here
    }
    // end::exceptions_eof[]
    done = true;
}

// Peer for the echo loop: send a message, read it back, then close so
// the session sees end-of-stream and terminates.
capy::task<>
echo_peer(corosio::tcp_socket& sock, std::string& reply, bool& done)
{
    co_await capy::write(sock, capy::const_buffer("echo", 4));
    char tmp[4] = {};
    auto [ec, n] = co_await capy::read(
        sock, capy::mutable_buffer(tmp, sizeof tmp));
    reply.assign(tmp, n);
    sock.close();
    done = true;
}

capy::task<>
read_exactly(
    corosio::tcp_socket& sock, capy::mutable_buffer buf, std::size_t& got)
{
    auto [ec, n] = co_await capy::read(sock, buf);
    got = n;
}

struct echo_server_test
{
    // Linger is disabled so close() performs a graceful shutdown and the
    // fragments observe eof rather than a reset.
    static std::pair<corosio::tcp_socket, corosio::tcp_socket>
    make_pair(corosio::io_context& ioc)
    {
        return corosio::test::make_socket_pair<
            corosio::tcp_socket, corosio::tcp_acceptor, false>(ioc);
    }

    void
    testComposedWrite()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        auto [a, b] = make_pair(ioc);

        bool done = false;
        std::size_t got = 0;
        char rx[4] = {};
        capy::run_async(ex)(composed_write_demo(
            a, capy::const_buffer("hi", 2), done));
        capy::run_async(ex)(read_exactly(
            b, capy::mutable_buffer(rx, sizeof rx), got));
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(got == 4);
        BOOST_TEST(std::memcmp(rx, "hihi", 4) == 0);
        a.close();
        b.close();
        ioc.restart();
        ioc.run();
    }

    void
    testAdvanceThenCheck()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        auto [a, b] = make_pair(ioc);

        bool session_done = false;
        bool peer_done = false;
        std::string reply;
        char storage[1024];
        capy::run_async(ex)(advance_then_check_session(
            a, capy::mutable_buffer(storage, sizeof storage), session_done));
        capy::run_async(ex)(echo_peer(b, reply, peer_done));
        ioc.run();

        BOOST_TEST(session_done);
        BOOST_TEST(peer_done);
        BOOST_TEST(reply == "echo");
        a.close();
        ioc.restart();
        ioc.run();
    }

    void
    testExceptionStyle()
    {
        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        auto [a, b] = make_pair(ioc);

        // Immediate close: the fragment's read fails with eof, which the
        // exception style surfaces as a throw.
        b.close();

        bool done = false;
        char storage[64];
        capy::run_async(ex)(exception_style_session(
            a, capy::mutable_buffer(storage, sizeof storage), done));
        ioc.run();

        BOOST_TEST(done);
        a.close();
        ioc.restart();
        ioc.run();
    }

    void
    run()
    {
        testComposedWrite();
        testAdvanceThenCheck();
        testExceptionStyle();
    }
};

} // namespace

TEST_SUITE(echo_server_test, "boost.corosio.doc.3a_echo_server");
