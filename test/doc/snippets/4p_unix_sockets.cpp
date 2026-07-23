//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4p.unix-sockets.adoc.

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
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/capy/buffers.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <unistd.h>
#endif

#include "test_suite.hpp"

namespace {

// The stream client/server fragments rendezvous on the page's
// literal path; stale files from crashed runs are unlinked first.
void
remove_stale(char const* path)
{
    std::remove(path);
}

// tag::server[]
capy::task<> server(corosio::io_context& ioc)
{
    corosio::local_stream_acceptor acc(ioc);
    acc.open();

    auto ec = acc.bind(corosio::local_endpoint("/tmp/my_app.sock"));
    if (ec) co_return;

    ec = acc.listen();
    if (ec) co_return;

    corosio::local_stream_socket peer(ioc);
    auto [accept_ec] = co_await acc.accept(peer);
    if (accept_ec) co_return;

    // peer is now connected — read and write as with tcp_socket
    char buf[1024];
    auto [read_ec, n] = co_await peer.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
}
// end::server[]

// tag::client[]
capy::task<> client(corosio::io_context& ioc)
{
    corosio::local_stream_socket s(ioc);

    // connect() opens the socket automatically
    auto [ec] = co_await s.connect(
        corosio::local_endpoint("/tmp/my_app.sock"));
    if (ec) co_return;

    char const msg[] = "hello";
    auto [wec, n] = co_await s.write_some(
        capy::const_buffer(msg, sizeof(msg)));
}
// end::client[]

#if BOOST_COROSIO_POSIX
void
unlink_then_bind(corosio::io_context& ioc, bool& bound)
{
    corosio::local_stream_acceptor acc(ioc);
    acc.open();
    // tag::unlink_bind[]
    ::unlink("/tmp/my_app.sock");  // remove stale socket
    acc.bind(corosio::local_endpoint("/tmp/my_app.sock"));
    // end::unlink_bind[]
    bound = std::filesystem::exists("/tmp/my_app.sock");
    acc.close();
    ::unlink("/tmp/my_app.sock");
}
#endif

capy::task<>
stream_pair(
    corosio::io_context& ioc, std::size_t& n_out, bool& matched)
{
    // tag::stream_pair[]
    corosio::local_stream_socket s1(ioc), s2(ioc);
    if (auto ec = corosio::connect_pair(s1, s2))
        throw std::system_error(ec, "connect_pair");

    // Data written to s1 can be read from s2, and vice versa.
    co_await s1.write_some(capy::const_buffer("ping", 4));

    char buf[16];
    auto [ec, n] = co_await s2.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    // buf contains "ping"
    // end::stream_pair[]
    n_out = n;
    matched = std::memcmp(buf, "ping", 4) == 0;
}

#if BOOST_COROSIO_POSIX

capy::task<>
datagram_connectionless(
    corosio::io_context& ioc, std::error_code& ec_out, std::size_t& n_out)
{
    char buf[64];
    // tag::datagram_connectionless[]
    corosio::local_datagram_socket s(ioc);
    s.open();
    s.bind(corosio::local_endpoint("/tmp/my_dgram.sock"));

    // Send to a specific peer
    co_await s.send_to(
        capy::const_buffer("hello", 5),
        corosio::local_endpoint("/tmp/peer.sock"));

    // Receive from any sender
    corosio::local_endpoint sender;
    auto [ec, n] = co_await s.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    // end::datagram_connectionless[]
    ec_out = ec;
    n_out = n;
}

capy::task<>
datagram_pair(
    corosio::io_context& ioc, std::size_t& n_out, bool& matched)
{
    char buf[64];
    // tag::datagram_pair[]
    corosio::local_datagram_socket s1(ioc), s2(ioc);
    if (auto ec = corosio::connect_pair(s1, s2))
        throw std::system_error(ec, "connect_pair");

    co_await s1.send(capy::const_buffer("msg", 3));

    auto [ec, n] = co_await s2.recv(
        capy::mutable_buffer(buf, sizeof(buf)));
    // end::datagram_pair[]
    n_out = n;
    matched = !ec && std::memcmp(buf, "msg", 3) == 0;
}

#endif // BOOST_COROSIO_POSIX

struct unix_sockets_test
{
    void
    testStreamClientServer()
    {
        remove_stale("/tmp/my_app.sock");

        corosio::io_context ioc;
        auto ex = ioc.get_executor();
        bool server_done = false;
        bool client_done = false;

        auto track = [](capy::task<> t, bool& done) -> capy::task<>
        {
            co_await std::move(t);
            done = true;
        };

        capy::run_async(ex)(track(server(ioc), server_done));
        capy::run_async(ex)(track(client(ioc), client_done));
        ioc.run();

        BOOST_TEST(server_done);
        BOOST_TEST(client_done);
        // The server bound the page's literal path
        BOOST_TEST(std::filesystem::exists("/tmp/my_app.sock"));
        remove_stale("/tmp/my_app.sock");
    }

#if BOOST_COROSIO_POSIX
    void
    testUnlinkBind()
    {
        corosio::io_context ioc;
        bool bound = false;
        unlink_then_bind(ioc, bound);
        BOOST_TEST(bound);
    }
#endif

    void
    testStreamPair()
    {
        corosio::io_context ioc;
        std::size_t n = 0;
        bool matched = false;
        capy::run_async(ioc.get_executor())(stream_pair(ioc, n, matched));
        ioc.run();
        BOOST_TEST_EQ(n, 4u);
        BOOST_TEST(matched);
    }

#if BOOST_COROSIO_POSIX
    void
    testDatagramConnectionless()
    {
        remove_stale("/tmp/my_dgram.sock");
        remove_stale("/tmp/peer.sock");

        corosio::io_context ioc;
        auto ex = ioc.get_executor();

        // A live peer bound to the page's literal path receives the
        // fragment's datagram and answers it.
        corosio::local_datagram_socket peer(ioc);
        peer.open();
        auto bec = peer.bind(corosio::local_endpoint("/tmp/peer.sock"));
        BOOST_TEST(!bec);

        std::error_code ec;
        std::size_t n = 0;
        capy::run_async(ex)(datagram_connectionless(ioc, ec, n));

        auto responder = [&peer]() -> capy::task<>
        {
            char buf[64];
            corosio::local_endpoint from;
            auto [rec, rn] = co_await peer.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), from);
            if (rec)
                co_return;
            co_await peer.send_to(capy::const_buffer(buf, rn), from);
        };
        capy::run_async(ex)(responder());

        ioc.run();

        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);

        peer.close();
        remove_stale("/tmp/my_dgram.sock");
        remove_stale("/tmp/peer.sock");
    }

    void
    testDatagramPair()
    {
        corosio::io_context ioc;
        std::size_t n = 0;
        bool matched = false;
        capy::run_async(ioc.get_executor())(datagram_pair(ioc, n, matched));
        ioc.run();
        BOOST_TEST_EQ(n, 3u);
        BOOST_TEST(matched);
    }
#endif

    void
    testEndpoints()
    {
        // tag::endpoints[]
        // Create from a path
        corosio::local_endpoint ep("/tmp/my_app.sock");

        // Query the path
        std::string_view path = ep.path();

        // Check if empty (unbound)
        bool bound = !ep.empty();
        // end::endpoints[]
        BOOST_TEST(path == "/tmp/my_app.sock");
        BOOST_TEST(bound);
    }

    void
    testAbstract()
    {
        // tag::abstract[]
        // Abstract socket — no file created
        corosio::local_endpoint ep(std::string_view("\0/my_app", 8));
        assert(ep.is_abstract());
        // end::abstract[]
        BOOST_TEST(ep.is_abstract());
    }

    void
    run()
    {
#if BOOST_COROSIO_POSIX
        testStreamClientServer();
#endif
#if BOOST_COROSIO_POSIX
        testUnlinkBind();
#endif
        testStreamPair();
#if BOOST_COROSIO_POSIX
        testDatagramConnectionless();
        testDatagramPair();
#endif
        testEndpoints();
        testAbstract();
    }
};

} // namespace

TEST_SUITE(unix_sockets_test, "boost.corosio.doc.4p_unix_sockets");
