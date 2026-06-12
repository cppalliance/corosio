//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_TLS_STREAM_TESTS_HPP
#define BOOST_COROSIO_TEST_TLS_STREAM_TESTS_HPP

#include "test_utils.hpp"
#include "stream_tests.hpp"
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/test/mocket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/test/fuse.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <cstddef>

#include "test_suite.hpp"

namespace boost::corosio::test {

// Max size variations: small sizes test chunked I/O behavior
inline constexpr std::array<std::size_t, 6> tls_max_sizes = {1,  5,    13,
                                                             64, 1024, 16384};

//
// Fuse Tests - test TLS behavior with chunked I/O
//

/** Test TLS handshake with max_size variations.

    Each max_size variation tests short reads/writes during handshake.
*/
template<typename StreamFactory>
void
testHandshakeFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            std::error_code client_ec;
            std::error_code server_ec;

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                client_ec = ec;
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                server_ec = ec;
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());

            ioc.run();

            BOOST_TEST(!client_ec);
            BOOST_TEST(!server_ec);

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
    }
}

/** Test TLS read/write with max_size variations.

    After a successful handshake, tests bidirectional data transfer.
    Test data size scales with max_size to keep tests fast.
*/
template<typename StreamFactory>
void
testReadWriteFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            auto test_data = corosio::test::scaled_test_data(max_size);

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                BOOST_TEST(!ec);
                if (ec)
                    co_return;

                // Write test data
                auto [ec2, n] = co_await client.write_some(
                    capy::const_buffer(test_data.data(), test_data.size()));
                BOOST_TEST(!ec2);
                if (ec2)
                    co_return;

                // Read echoed data
                std::string buf(test_data.size(), '\0');
                auto [ec3, n3] = co_await client.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!ec3);
                if (!ec3)
                    BOOST_TEST(buf.substr(0, n3) == test_data.substr(0, n3));
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                BOOST_TEST(!ec);
                if (ec)
                    co_return;

                // Read data from client
                std::string buf(test_data.size(), '\0');
                auto [ec2, n] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!ec2);
                if (ec2)
                    co_return;

                // Echo it back
                (void)co_await server.write_some(
                    capy::const_buffer(buf.data(), n));
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());
            ioc.run();

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
    }
}

/** Test TLS shutdown with max_size variations.

    After handshake, tests graceful shutdown.
*/
template<typename StreamFactory>
void
testShutdownFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        // Skip very small max_size for shutdown tests
        // (shutdown is just close_notify, not much data)
        if (max_size < 64)
            continue;

        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                BOOST_TEST(!ec);
                if (ec)
                    co_return;

                // Initiate shutdown
                (void)co_await client.shutdown();
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                BOOST_TEST(!ec);
                if (ec)
                    co_return;

                // Read until EOF (from shutdown)
                char buf[32];
                (void)co_await server.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                // Close socket to unblock client shutdown
                m2.close();
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());
            ioc.run();

            m1.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
    }
}

//
// Success/Failure Tests
//

/** Test TLS success cases with certificate verification.

    These tests run without fuse injection to verify basic functionality
    works correctly with different certificate configurations.

    @param modes  Array of context_mode values to test
*/
template<typename StreamFactory, std::size_t N>
void
testSuccessCases(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test TLS failure cases.

    Tests various certificate validation failures.
*/
template<typename StreamFactory>
void
testFailureCases(StreamFactory make_stream)
{
    io_context ioc;

    // Client verifies, server has no cert
    {
        auto client_ctx = make_client_context();
        auto server_ctx = make_anon_context();
        server_ctx.set_ciphersuites(""); // NOLINT(bugprone-unused-return-value)
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
        ioc.restart();
    }

    // Client trusts wrong CA
    {
        auto client_ctx = make_wrong_ca_context();
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
        ioc.restart();
    }
}

/** Test TLS shutdown with proper close_notify exchange. */
template<typename StreamFactory, std::size_t N>
void
testTlsShutdown(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_shutdown_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test stream truncation detection. */
template<typename StreamFactory, std::size_t N>
void
testStreamTruncated(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_truncation_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test stop token cancellation. */
template<typename StreamFactory>
void
testStopTokenCancellation(StreamFactory make_stream)
{
    // Cancel during handshake
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_stop_token_handshake_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Cancel during read
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_stop_token_read_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Cancel during write
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_stop_token_write_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test socket error propagation. */
template<typename StreamFactory>
void
testSocketErrorPropagation(StreamFactory make_stream)
{
    // socket.cancel() while TLS blocked on socket I/O
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_socket_cancel_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Connection reset during handshake
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_connection_reset_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test certificate validation. */
template<typename StreamFactory>
void
testCertificateValidation(StreamFactory make_stream)
{
    // Untrusted CA
    {
        io_context ioc;
        auto client_ctx = make_untrusted_ca_client_context();
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Expired certificate
    {
        io_context ioc;
        auto client_ctx = make_expired_client_context();
        auto server_ctx = make_expired_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test SNI (Server Name Indication). */
template<typename StreamFactory>
void
testSni(StreamFactory make_stream)
{
    // Correct hostname succeeds
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        client_ctx.set_hostname("www.example.com");
        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Wrong hostname fails
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        client_ctx.set_hostname("wrong.example.com");
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test SNI callback. */
template<typename StreamFactory>
void
testSniCallback(StreamFactory make_stream)
{
    // SNI callback accepts hostname
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        client_ctx.set_hostname("www.example.com");

        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [](std::string_view hostname) -> bool {
                return hostname == "www.example.com";
            });

        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // SNI callback rejects hostname
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        client_ctx.set_hostname("www.example.com");

        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [](std::string_view hostname) -> bool {
                return hostname == "api.example.com";
            });

        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Client sends no SNI: the server callback is never consulted and
    // the handshake still succeeds (no-acknowledgement, not an error).
    {
        io_context ioc;
        auto client_ctx = make_client_context();

        bool invoked    = false;
        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [&invoked](std::string_view) -> bool {
                invoked = true;
                return true;
            });

        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(!invoked);
    }
}

/** Test move construction and move assignment of a live stream.

    The moved-into stream must keep working after a completed
    handshake; the moved-from stream must be safely destroyable.
*/
template<typename StreamFactory>
void
testMoveSemantics(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client_pre = make_stream(m1, client_ctx);
    auto client_orig{std::move(client_pre)};
    auto server       = make_stream(m2, server_ctx);
    using stream_type = std::remove_reference_t<decltype(server)>;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec] = co_await client_orig.handshake(stream_type::client);
        BOOST_TEST(!ec);
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(stream_type::server);
        BOOST_TEST(!ec);
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();
    ioc.restart();

    // Move-construct from the handshaked stream, then move-assign
    // over a freshly constructed target (its impl must be released).
    auto client_moved = std::move(client_orig);
    auto client       = make_stream(m1, client_ctx);
    client            = std::move(client_moved);

    bool write_done = false, read_done = false;
    auto writer = [&]() -> capy::task<> {
        auto [ec, n] = co_await client.write_some(
            capy::const_buffer("moved", 5));
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);
        write_done = true;
    };
    auto reader = [&]() -> capy::task<> {
        char buf[16];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);
        read_done = true;
    };
    capy::run_async(ioc.get_executor())(writer());
    capy::run_async(ioc.get_executor())(reader());
    ioc.run();

    BOOST_TEST(write_done);
    BOOST_TEST(read_done);
}

/** Test reads and shutdown after the peer vanishes without close_notify.

    The transport closing under an established TLS session must surface
    `stream_truncated` to a pending read, and a subsequent shutdown()
    must complete instead of hanging.
*/
template<typename StreamFactory>
void
testAbruptClose(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client       = make_stream(m1, client_ctx);
    auto server       = make_stream(m2, server_ctx);
    using stream_type = std::remove_reference_t<decltype(server)>;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(stream_type::client);
        BOOST_TEST(!ec);
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(stream_type::server);
        BOOST_TEST(!ec);
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();
    ioc.restart();

    // Server's transport disappears without a TLS shutdown.
    m2.close();

    bool read_done = false;
    std::error_code read_ec;
    auto reader = [&]() -> capy::task<> {
        char buf[16];
        auto [ec, n] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        (void)n;
        read_ec   = ec;
        read_done = true;
    };
    capy::run_async(ioc.get_executor())(reader());
    ioc.run();
    ioc.restart();

    BOOST_TEST(read_done);
    BOOST_TEST(read_ec == capy::error::stream_truncated);

    // Shutdown must complete; the missing close_notify reply is
    // normalized, not reported as a transport error.
    bool shutdown_done = false;
    auto closer = [&]() -> capy::task<> {
        (void)co_await client.shutdown();
        shutdown_done = true;
    };
    capy::run_async(ioc.get_executor())(closer());
    ioc.run();

    BOOST_TEST(shutdown_done);
}

/** Test handshaking with a password-protected server key.

    The server private key is encrypted; loading it must route through
    the context's password callback.

    @param expect_success Whether the handshake must succeed. Pass
        `false` for TLS builds whose encrypted-key support is a
        compile-time feature (wolfSSL): the key-loading path and the
        password callback still run, but the handshake may fail
        cleanly instead of completing.
*/
template<typename StreamFactory>
void
testEncryptedKey(StreamFactory make_stream, bool expect_success = true)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx       = make_client_context();
    bool callback_invoked = false;
    auto server_ctx = make_encrypted_key_server_context(callback_invoked);

    auto client       = make_stream(m1, client_ctx);
    auto server       = make_stream(m2, server_ctx);
    using stream_type = std::remove_reference_t<decltype(server)>;

    bool client_done = false, server_done = false;
    bool failsafe_hit = false;
    std::error_code client_ec, server_ec;

    // If the TLS build cannot decrypt the key the handshake stalls
    // with both sides waiting; tear the transport down instead of
    // hanging the suite.
    timer failsafe(ioc);
    failsafe.expires_after(std::chrono::seconds(5));

    auto client_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await client.handshake(stream_type::client);
        client_ec   = ec;
        client_done = true;
        if (server_done)
            failsafe.cancel();
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await server.handshake(stream_type::server);
        server_ec   = ec;
        server_done = true;
        if (client_done)
            failsafe.cancel();
    };
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await failsafe.wait();
        if (!ec)
        {
            failsafe_hit = true;
            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    capy::run_async(ioc.get_executor())(failsafe_task());
    ioc.run();

    BOOST_TEST(client_done);
    BOOST_TEST(server_done);
    BOOST_TEST(callback_invoked);
    if (expect_success)
    {
        BOOST_TEST(!failsafe_hit);
        BOOST_TEST(!client_ec);
        BOOST_TEST(!server_ec);
    }
}

/** Test that a context with unparseable credentials fails the
    handshake with an error instead of crashing or hanging.
*/
template<typename StreamFactory>
void
testInvalidContextHandshake(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();

    tls_context server_ctx;
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.use_certificate("not a certificate", tls_file_format::pem);
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.use_private_key("not a key", tls_file_format::pem);
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.set_verify_mode(tls_verify_mode::none);

    auto client       = make_stream(m1, client_ctx);
    auto server       = make_stream(m2, server_ctx);
    using stream_type = std::remove_reference_t<decltype(server)>;

    bool client_done = false, server_done = false;
    std::error_code client_ec, server_ec;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await client.handshake(stream_type::client);
        client_ec   = ec;
        client_done = true;
        // Unblock the server if it is still waiting on the transport.
        m1.close(); // NOLINT(bugprone-unused-return-value)
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await server.handshake(stream_type::server);
        server_ec   = ec;
        server_done = true;
        m2.close(); // NOLINT(bugprone-unused-return-value)
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();

    BOOST_TEST(client_done);
    BOOST_TEST(server_done);
    // At least one side must report the failure.
    BOOST_TEST(!!client_ec || !!server_ec);
}

/** Test mutual TLS (mTLS). */
template<typename StreamFactory>
void
testMtls(StreamFactory make_stream)
{
    // mTLS success
    {
        io_context ioc;
        auto client_ctx = make_mtls_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // mTLS failure - no client cert
    {
        io_context ioc;
        auto client_ctx = make_chain_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // mTLS failure - wrong client cert
    {
        io_context ioc;
        auto client_ctx = make_invalid_mtls_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

//
// Reset Tests
//

/** Test explicit reset() between TLS sessions.

    Verifies that calling reset() after shutdown allows the
    same stream objects to perform a new handshake and data
    transfer. Two full rounds on the same stream pair.
*/
template<typename StreamFactory, std::size_t N>
void
testReset(StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto [client_ctx, server_ctx] = make_contexts(mode);
        auto client                   = make_stream(m1, client_ctx);
        auto server                   = make_stream(m2, server_ctx);

        auto do_round = [&](std::string const& msg) {
            std::error_code client_ec;
            std::error_code server_ec;

            // Handshake
            auto hs_client = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                client_ec = ec;
            };
            auto hs_server = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                server_ec = ec;
            };

            capy::run_async(ioc.get_executor())(hs_client());
            capy::run_async(ioc.get_executor())(hs_server());
            ioc.run();
            ioc.restart();

            BOOST_TEST(!client_ec);
            BOOST_TEST(!server_ec);
            if (client_ec || server_ec)
                return;

            // Data transfer
            auto xfer = [&]() -> capy::task<> {
                // Client writes
                auto [wec, wn] = co_await client.write_some(
                    capy::const_buffer(msg.data(), msg.size()));
                BOOST_TEST(!wec);
                if (wec)
                    co_return;

                // Server reads
                std::string buf(msg.size(), '\0');
                auto [rec, rn] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!rec);
                if (!rec)
                    BOOST_TEST(buf.substr(0, rn) == msg.substr(0, rn));
            };
            capy::run_async(ioc.get_executor())(xfer());
            ioc.run();
            ioc.restart();

            // Shutdown both sides concurrently
            auto sd_client = [&]() -> capy::task<> {
                (void)co_await client.shutdown();
            };
            auto sd_server = [&]() -> capy::task<> {
                // Read until close_notify, then send ours
                char drain[32];
                (void)co_await server.read_some(
                    capy::mutable_buffer(drain, sizeof(drain)));
                (void)co_await server.shutdown();
            };

            capy::run_async(ioc.get_executor())(sd_client());
            capy::run_async(ioc.get_executor())(sd_server());
            ioc.run();
            ioc.restart();
        };

        // Round 1
        do_round("hello1");

        // Explicit reset
        client.reset();
        server.reset();

        // Round 2
        do_round("hello2");

        m1.close(); // NOLINT(bugprone-unused-return-value)
        m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Test implicit reset via handshake().

    Verifies that calling handshake() on a previously-used stream
    automatically resets, without an explicit reset() call.
*/
template<typename StreamFactory, std::size_t N>
void
testResetViaHandshake(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto [client_ctx, server_ctx] = make_contexts(mode);
        auto client                   = make_stream(m1, client_ctx);
        auto server                   = make_stream(m2, server_ctx);

        auto do_round = [&](std::string const& msg) {
            std::error_code client_ec;
            std::error_code server_ec;

            auto hs_client = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                client_ec = ec;
            };
            auto hs_server = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                server_ec = ec;
            };

            capy::run_async(ioc.get_executor())(hs_client());
            capy::run_async(ioc.get_executor())(hs_server());
            ioc.run();
            ioc.restart();

            BOOST_TEST(!client_ec);
            BOOST_TEST(!server_ec);
            if (client_ec || server_ec)
                return;

            auto xfer = [&]() -> capy::task<> {
                auto [wec, wn] = co_await client.write_some(
                    capy::const_buffer(msg.data(), msg.size()));
                BOOST_TEST(!wec);
                if (wec)
                    co_return;

                std::string buf(msg.size(), '\0');
                auto [rec, rn] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!rec);
                if (!rec)
                    BOOST_TEST(buf.substr(0, rn) == msg.substr(0, rn));
            };
            capy::run_async(ioc.get_executor())(xfer());
            ioc.run();
            ioc.restart();

            auto sd_client = [&]() -> capy::task<> {
                (void)co_await client.shutdown();
            };
            auto sd_server = [&]() -> capy::task<> {
                char drain[32];
                (void)co_await server.read_some(
                    capy::mutable_buffer(drain, sizeof(drain)));
                (void)co_await server.shutdown();
            };

            capy::run_async(ioc.get_executor())(sd_client());
            capy::run_async(ioc.get_executor())(sd_server());
            ioc.run();
            ioc.restart();
        };

        // Round 1
        do_round("round1");

        // No explicit reset -- handshake() should auto-reset

        // Round 2
        do_round("round2");

        m1.close(); // NOLINT(bugprone-unused-return-value)
        m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Test reset with fuse/max_size variations.

    Stresses chunked I/O across reset boundaries.
*/
template<typename StreamFactory>
void
testResetFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        if (max_size < 64)
            continue;

        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            // Round 1
            {
                std::error_code cec, sec;
                auto hsc = [&]() -> capy::task<> {
                    auto [ec] = co_await client.handshake(tls_stream::client);
                    cec       = ec;
                };
                auto hss = [&]() -> capy::task<> {
                    auto [ec] = co_await server.handshake(tls_stream::server);
                    sec       = ec;
                };
                capy::run_async(ioc.get_executor())(hsc());
                capy::run_async(ioc.get_executor())(hss());
                ioc.run();
                ioc.restart();
                BOOST_TEST(!cec);
                BOOST_TEST(!sec);
                if (cec || sec)
                    return;

                // Shutdown
                auto sdc = [&]() -> capy::task<> {
                    (void)co_await client.shutdown();
                };
                auto sds = [&]() -> capy::task<> {
                    char drain[32];
                    (void)co_await server.read_some(
                        capy::mutable_buffer(drain, sizeof(drain)));
                    (void)co_await server.shutdown();
                };
                capy::run_async(ioc.get_executor())(sdc());
                capy::run_async(ioc.get_executor())(sds());
                ioc.run();
                ioc.restart();
            }

            // Reset both
            client.reset();
            server.reset();

            // Round 2
            {
                std::error_code cec, sec;
                auto hsc = [&]() -> capy::task<> {
                    auto [ec] = co_await client.handshake(tls_stream::client);
                    cec       = ec;
                };
                auto hss = [&]() -> capy::task<> {
                    auto [ec] = co_await server.handshake(tls_stream::server);
                    sec       = ec;
                };
                capy::run_async(ioc.get_executor())(hsc());
                capy::run_async(ioc.get_executor())(hss());
                ioc.run();
                ioc.restart();
                BOOST_TEST(!cec);
                BOOST_TEST(!sec);
            }

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        });
    }
}

} // namespace boost::corosio::test

#endif
