//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/5.testing/5a.mocket.adoc.

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
#include <boost/corosio/test/mocket.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <boost/corosio/backend.hpp>
#include <boost/corosio/native/native.hpp>

#include <string_view>
#include <type_traits>

#include "test_suite.hpp"

namespace {

// The page's `backend` placeholder: this platform's native backend tag.
#if BOOST_COROSIO_HAS_EPOLL
constexpr auto backend = corosio::epoll;
#elif BOOST_COROSIO_HAS_KQUEUE
constexpr auto backend = corosio::kqueue;
#elif BOOST_COROSIO_HAS_IOCP
constexpr auto backend = corosio::iocp;
#endif

struct mocket_page_test
{
    void
    testCreating()
    {
        // tag::creating[]
        corosio::io_context ioc;

        auto [m, peer] = corosio::test::make_mocket_pair(ioc);
        // end::creating[]
        // "Both are open and immediately usable."
        BOOST_TEST(m.is_open());
        BOOST_TEST(peer.is_open());
        BOOST_TEST(!m.close());
        peer.close();
    }

    void
    testProvide()
    {
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // tag::provide[]
        m.provide("HTTP/1.1 200 OK\r\n\r\nHello");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            char buf[64] = {};
            auto [ec, n] = co_await m_ref.read_some(capy::make_buffer(buf));
            // buf[0..n] == "HTTP/1.1 200 OK\r\n\r\nHello"
        };
        // end::provide[]
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();
        ioc.restart();

        // A clean close proves the staged bytes were fully consumed.
        BOOST_TEST(!m.close());
        peer.close();
    }

    void
    testExpect()
    {
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // tag::expect[]
        m.expect("GET / HTTP/1.1\r\n\r\n");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            auto [ec, n] = co_await m_ref.write_some(
                capy::const_buffer("GET / HTTP/1.1\r\n\r\n", 18));
            // ec is empty; n == 18
        };
        // end::expect[]
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();
        ioc.restart();

        // A clean close proves the expected bytes were all written.
        BOOST_TEST(!m.close());
        peer.close();
    }

    void
    testChunked()
    {
        corosio::io_context ioc;
        // tag::chunked[]
        // max_read_size = 4, max_write_size = 3 force short transfers.
        auto [m, peer] = corosio::test::make_mocket_pair(ioc, {}, 4, 3);

        m.provide("0123456789");
        m.expect("abcdef");

        auto task = [](corosio::test::mocket& m_ref) -> capy::task<> {
            char buf[16] = {};
            auto [rec, rn] = co_await m_ref.read_some(capy::make_buffer(buf));
            // rn == 4 ("0123")

            auto [wec, wn] = co_await m_ref.write_some(
                capy::const_buffer("abcdef", 6));
            // wn == 3 (matched "abc")
        };
        // end::chunked[]
        capy::run_async(ioc.get_executor())(task(m));
        ioc.run();
        ioc.restart();

        // Draining the leftovers proves the shown task moved exactly
        // 4 bytes out and matched exactly 3 bytes in.
        auto drain = [](corosio::test::mocket& m_ref) -> capy::task<> {
            char buf[16] = {};
            auto [ec1, n1] = co_await m_ref.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "4567");
            auto [ec2, n2] = co_await m_ref.read_some(capy::make_buffer(buf));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(std::string_view(buf, n2), "89");
            auto [ec3, n3] = co_await m_ref.write_some(
                capy::const_buffer("def", 3));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(n3, 3u);
        };
        capy::run_async(ioc.get_executor())(drain(m));
        ioc.run();
        ioc.restart();

        BOOST_TEST(!m.close());
        peer.close();
    }

    void
    testCloseCheck()
    {
        corosio::io_context ioc;
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        // Deliberately leave staged data unconsumed so close() reports it.
        m.provide("unread");

        // tag::close_check[]
        auto ec = m.close();
        if (ec == capy::error::test_failure)
        {
            // Either provide() data was never read,
            // or expect() data was never written.
        }
        // end::close_check[]
        BOOST_TEST(ec == capy::error::test_failure);
        peer.close();
    }

    void
    testNative()
    {
        // tag::native[]
        using socket_type   = corosio::native_tcp_socket<backend>;
        using acceptor_type = corosio::native_tcp_acceptor<backend>;
        using mocket_type   = corosio::test::basic_mocket<socket_type>;

        corosio::io_context ioc(backend);

        auto [m, peer] =
            corosio::test::make_mocket_pair<socket_type, acceptor_type>(ioc);
        // end::native[]
        static_assert(std::is_same_v<decltype(m), mocket_type>);
        BOOST_TEST(m.is_open());
        BOOST_TEST(peer.is_open());
        BOOST_TEST(!m.close());
        peer.close();
    }

    void
    testSocketAccess()
    {
        corosio::io_context ioc;
        // tag::socket_access[]
        auto [m, peer] = corosio::test::make_mocket_pair(ioc);

        corosio::tcp_socket& under = m.socket();
        // Pass `under` into a TLS stream, a custom framing layer, etc.
        // end::socket_access[]
        BOOST_TEST(under.is_open());
        BOOST_TEST(!m.close());
        // `under` is the mocket's own socket, so it closed with it.
        BOOST_TEST(!under.is_open());
        peer.close();
    }

    void
    run()
    {
        testCreating();
        testProvide();
        testExpect();
        testChunked();
        testCloseCheck();
        testNative();
        testSocketAccess();
    }
};

} // namespace

TEST_SUITE(mocket_page_test, "boost.corosio.doc.5a_mocket");
