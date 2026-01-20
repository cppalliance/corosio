//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tls/openssl_stream.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {

inline capy::task<>
test_stream(io_stream& a, io_stream& b)
{
    char buf[32] = {};

    // Write from a, read from b
    auto [ec1, n1] = co_await a.write_some(
        capy::const_buffer("hello", 5));
    BOOST_TEST(!ec1);
    BOOST_TEST_EQ(n1, 5u);

    auto [ec2, n2] = co_await b.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    BOOST_TEST(!ec2);
    BOOST_TEST_EQ(n2, 5u);
    BOOST_TEST_EQ(std::string_view(buf, n2), "hello");

    // Write from b, read from a
    auto [ec3, n3] = co_await b.write_some(
        capy::const_buffer("world", 5));
    BOOST_TEST(!ec3);
    BOOST_TEST_EQ(n3, 5u);

    auto [ec4, n4] = co_await a.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    BOOST_TEST(!ec4);
    BOOST_TEST_EQ(n4, 5u);
    BOOST_TEST_EQ(std::string_view(buf, n4), "world");
}

struct openssl_stream_test
{
#ifdef BOOST_COROSIO_HAS_OPENSSL
    void
    testConnect()
    {
        io_context ioc;
        auto [s1, s2] = test::make_socket_pair(ioc);

        tls::context ctx;
        ctx.set_verify_mode(tls::verify_mode::none);
        ctx.set_ciphersuites("aNULL:eNULL:@SECLEVEL=0");

        openssl_stream client(s1, ctx);
        openssl_stream server(s2, ctx);

        // Client handshake coroutine
        capy::run_async(ioc.get_executor())(
            [](openssl_stream& c) -> capy::task<>
            {
                auto [ec] = co_await c.handshake(tls_stream::client);
                BOOST_TEST(!ec);
            }(client));

        // Server handshake coroutine
        capy::run_async(ioc.get_executor())(
            [](openssl_stream& s) -> capy::task<>
            {
                auto [ec] = co_await s.handshake(tls_stream::server);
                BOOST_TEST(!ec);
            }(server));

        // Run until handshakes complete
        ioc.run();
        ioc.restart();

        // Test bidirectional communication
        capy::run_async(ioc.get_executor())(
            [](openssl_stream& c, openssl_stream& s) -> capy::task<>
            {
                co_await test_stream(c, s);
            }(client, server));

        ioc.run();
        ioc.restart();

        // Graceful TLS shutdown (both sides concurrently)
        capy::run_async(ioc.get_executor())(
            [](openssl_stream& c) -> capy::task<>
            {
                auto [ec] = co_await c.shutdown();
                BOOST_TEST(!ec);
            }(client));

        capy::run_async(ioc.get_executor())(
            [](openssl_stream& s) -> capy::task<>
            {
                auto [ec] = co_await s.shutdown();
                BOOST_TEST(!ec);
            }(server));

        ioc.run();

        s1.close();
        s2.close();
    }
#endif

    void
    run()
    {
#ifdef BOOST_COROSIO_HAS_OPENSSL
        testConnect();
#endif
    }
};

TEST_SUITE(openssl_stream_test, "boost.corosio.openssl_stream");

} // namespace corosio
} // namespace boost
