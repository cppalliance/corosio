//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/test/mocket.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/test/fuse.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <cstddef>
#include <stop_token>

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

/** Test cancellation during shutdown (cppalliance/corosio#301).

    Exercises both cancellation representations (stop token and socket cancel),
    which reach the shutdown read as different error codes; both must surface
    cond::canceled.
*/
template<typename StreamFactory>
void
testShutdownCancel(StreamFactory make_stream)
{
    for (auto mode : {shutdown_cancel_mode::socket_cancel,
                      shutdown_cancel_mode::stop_token})
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_shutdown_cancel_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream, mode);
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

/** A freshly constructed stream reports no negotiated ALPN protocol.

    The accessor must return an empty view before any handshake, on
    every backend (including builds without ALPN support).
*/
template<typename StreamFactory>
void
testAlpnAccessorEmpty(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);
    auto ctx      = make_client_context();
    auto stream   = make_stream(m1, ctx);
    BOOST_TEST(stream.alpn_protocol().empty());
    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Test CRL-based revocation.

    The server presents a leaf that a CRL revokes.

    When @p crl_supported (OpenSSL, or WolfSSL built with HAVE_CRL):
      1. hard_fail with the CRL loaded rejects the revoked leaf.
      2. soft_fail with no CRL loaded accepts (status unknown is allowed).
    Otherwise (WolfSSL without HAVE_CRL): any revocation request fails the
    handshake with function_not_supported rather than skip the check.

    On every build, a CRL that parses as neither PEM nor DER fails the
    handshake closed rather than being silently dropped (which would let
    soft_fail accept a peer the missing CRL might have revoked).
*/
template<typename StreamFactory>
void
testCrlRevocation(StreamFactory make_stream, bool crl_supported)
{
    auto revoked_server = []() {
        tls_context ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_certificate(revoked_leaf_cert_pem, tls_file_format::pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_private_key(revoked_leaf_key_pem, tls_file_format::pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.set_verify_mode(tls_verify_mode::none);
        return ctx;
    };
    auto revoking_client = [](tls_revocation_policy policy, bool load_crl) {
        tls_context ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.add_certificate_authority(root_ca_cert_pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.set_verify_mode(tls_verify_mode::peer);
        if (load_crl)
            ctx.add_crl(revoked_crl_pem); // NOLINT(bugprone-unused-return-value)
        ctx.set_revocation_policy(policy);
        return ctx;
    };

    if (crl_supported)
    {
        // 1. hard_fail + CRL -> revoked leaf rejected.
        {
            io_context ioc;
            auto client_ctx =
                revoking_client(tls_revocation_policy::hard_fail, true);
            auto server_ctx = revoked_server();
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }
        // 2. soft_fail + no CRL -> unknown status accepted.
        {
            io_context ioc;
            auto client_ctx =
                revoking_client(tls_revocation_policy::soft_fail, false);
            auto server_ctx = revoked_server();
            run_tls_test(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }
    }
    else
    {
        io_context ioc;
        auto client_ctx =
            revoking_client(tls_revocation_policy::hard_fail, true);
        auto server_ctx = revoked_server();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // A supplied CRL that parses as neither PEM nor DER must fail the
    // handshake, never silently downgrade to accepting the peer. This holds
    // on every build: a HAVE_CRL backend rejects the unparseable CRL, and a
    // backend without CRL support rejects any revocation request outright.
    // Uses soft_fail specifically: without the fail-closed guard, soft_fail
    // would treat the missing (dropped) CRL as "status unknown" and accept.
    {
        io_context ioc;
        tls_context client_ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.add_certificate_authority(root_ca_cert_pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_mode(tls_verify_mode::peer);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.add_crl("this is not a valid PEM or DER CRL");
        client_ctx.set_revocation_policy(tls_revocation_policy::soft_fail);
        auto server_ctx = revoked_server();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // A CRL supplied while the policy stays disabled is inert by contract
    // (consulted only when the policy is not disabled). This config must
    // work on every build — including a WolfSSL build without HAVE_CRL,
    // which must not reject a revocation feature that was never requested.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.add_crl(revoked_crl_pem); // policy left disabled
        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test loading server credentials from a PKCS#12 bundle.

    1. A server context whose cert and key come from a PKCS#12 blob
       completes a handshake with a client that trusts the CA.
    2. A wrong passphrase loads no credentials, so the handshake fails.
    3. A bundle takes precedence over discrete cert/key fields (which are
       ignored when a bundle is present).
    4. A client whose bundle fails to decode fails the handshake closed
       rather than silently proceeding with no credential (fail-open mTLS).

    Both backends decode PKCS#12 natively (OpenSSL PKCS12_parse, WolfSSL
    wc_PKCS12_parse), so no capability branching is needed.
*/
template<typename StreamFactory>
void
testPkcs12(StreamFactory make_stream)
{
    std::string_view const p12(
        reinterpret_cast<char const*>(server_p12), sizeof(server_p12));

    // 1. Correct passphrase: credentials load, handshake succeeds.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.use_pkcs12(p12, p12_password);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_verify_mode(tls_verify_mode::none);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Wrong passphrase: nothing loads, handshake fails.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.use_pkcs12(p12, "wrong-password");
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_verify_mode(tls_verify_mode::none);
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. A PKCS#12 bundle takes precedence over discrete cert/key fields.
    //    The bundle holds a valid, trusted credential; the discrete cert is
    //    an expired self-signed one the client would reject. The handshake
    //    succeeds only if the bundle's credential is used — i.e. the discrete
    //    fields are ignored when a bundle is present.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.use_pkcs12(p12, p12_password);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.use_certificate(expired_cert_pem, tls_file_format::pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.use_private_key(expired_key_pem, tls_file_format::pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_verify_mode(tls_verify_mode::none);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 4. A client whose PKCS#12 credential fails to decode must fail closed,
    //    not silently proceed with no certificate. The server verifies the
    //    peer with `peer` (not `require_peer`), so a client that lost its
    //    identity would otherwise complete the handshake — the fail-open the
    //    review flagged. A wrong passphrase makes the bundle fail to parse.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.use_pkcs12(p12, "wrong-password");
        auto server_ctx = make_server_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_verify_mode(tls_verify_mode::peer);
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that the intermediate chain inside a PKCS#12 bundle is loaded
    and sent during the handshake.

    The server loads a bundle containing leaf + key + intermediate. The
    client trusts only the root CA, so verification succeeds only if the
    server presents the intermediate — i.e. the bundle's chain was loaded,
    not just the leaf.
*/
template<typename StreamFactory>
void
testPkcs12Chain(StreamFactory make_stream)
{
    std::string_view const p12(
        reinterpret_cast<char const*>(server_chain_p12),
        sizeof(server_chain_p12));

    io_context ioc;
    auto client_ctx = make_rootonly_client_context();
    tls_context server_ctx;
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.use_pkcs12(p12, p12_password);
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.set_verify_mode(tls_verify_mode::none);
    run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Certificate-chain delivery: the server must send its intermediate.

    Both backends load a full chain (leaf + intermediate) and send the
    intermediate during the handshake, so a client that trusts only the
    root CA can complete the path. A server that presents only its leaf
    cannot, since the client lacks the intermediate to bridge to the root.

    1. Server sends the full chain, client trusts only the root -> succeeds.
    2. Server sends the leaf only, client trusts only the root -> fails.

    The intermediate carries a critical basicConstraints extension so that
    WolfSSL (which enforces RFC 5280 strictly) accepts it as a CA, matching
    OpenSSL.
*/
template<typename StreamFactory>
void
testCertificateChain(StreamFactory make_stream)
{
    // Server sends the full chain; client trusting only the root succeeds.
    {
        io_context ioc;
        auto client_ctx = make_rootonly_client_context();
        auto server_ctx = make_fullchain_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Server sends only the leaf; the client cannot build the path -> fails.
    {
        io_context ioc;
        auto client_ctx = make_rootonly_client_context();
        auto server_ctx = make_chain_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** set_default_verify_paths() applies cleanly on top of an explicit CA.

    Both backends can add the system trust store (OpenSSL:
    SSL_CTX_set_default_verify_paths; WolfSSL: wolfSSL_CTX_load_system_CA_certs
    where the build enables it, otherwise a no-op). Behaviorally exercising
    the system store would require redirecting it per-platform, which is not
    portable across the CI matrix, so this asserts only that the call path
    runs cleanly: a client trusting the test CA explicitly *and* adding the
    system store still completes the handshake. The load-from-path mechanism
    is covered behaviorally by each backend's add_verify_path test.
*/
template<typename StreamFactory>
void
testDefaultVerifyPaths(StreamFactory make_stream)
{
    io_context ioc;
    auto client_ctx = make_client_context();
    // Adding the system store on top of the explicit CA must not break
    // context creation or verification.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    client_ctx.set_default_verify_paths();

    auto server_ctx = make_server_context();
    run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Test that TLS 1.3 cipher suite selection is applied.

    1. Both peers restricted to @p suite_a -> handshake succeeds.
    2. Client restricted to @p suite_a, server to @p suite_b (disjoint)
       -> no common suite, handshake fails.
    3. An unparseable cipher string fails the handshake closed (setup_failed_)
       rather than silently reverting to the default suites.

    Suite names differ between backends (OpenSSL `TLS_AES_128_GCM_SHA256`
    vs WolfSSL `TLS13-AES128-GCM-SHA256`), so the caller supplies them.
*/
template<typename StreamFactory>
void
testCiphersuitesTls13(
    StreamFactory make_stream, char const* suite_a, char const* suite_b)
{
    auto make_ctx = [&](auto base, char const* suite) {
        auto ctx = base();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.set_min_protocol_version(tls_version::tls_1_3);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.set_ciphersuites_tls13(suite);
        return ctx;
    };

    // 1. Matching suite succeeds.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, suite_a);
        auto server_ctx = make_ctx(make_server_context, suite_a);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Disjoint suites fail.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, suite_a);
        auto server_ctx = make_ctx(make_server_context, suite_b);
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. A cipher string the library rejects fails closed, rather than
    //    silently falling back to the default suites.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, "not-a-real-suite");
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that protocol version bounds are enforced.

    1. Both peers pinned to TLS 1.3 -> handshake succeeds.
    2. Client capped at TLS 1.2 while the server requires TLS 1.3 ->
       no common version, handshake fails.

    Both backends support version bounds natively, so no capability
    branching is needed.
*/
template<typename StreamFactory>
void
testProtocolVersion(StreamFactory make_stream)
{
    // 1. TLS 1.3 on both sides succeeds.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_min_protocol_version(tls_version::tls_1_3);
        auto server_ctx = make_server_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_min_protocol_version(tls_version::tls_1_3);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Version window mismatch fails: client max 1.2, server min 1.3.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_max_protocol_version(tls_version::tls_1_2);
        auto server_ctx = make_server_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        server_ctx.set_min_protocol_version(tls_version::tls_1_3);
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. An inverted window on one context (min 1.3 > max 1.2) admits no
    //    protocol; the handshake must fail closed rather than silently
    //    negotiate an unexpected version.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_min_protocol_version(tls_version::tls_1_3);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_max_protocol_version(tls_version::tls_1_2);
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test ALPN negotiation end-to-end.

    Client and server both offer `{ "h2", "http/1.1" }`.

    @param alpn_supported Whether the backend build can negotiate ALPN.
        When true, the handshake succeeds and both peers report `"h2"`.
        When false (e.g. WolfSSL without `HAVE_ALPN`), offering ALPN must
        fail the handshake with `function_not_supported` rather than
        silently negotiate nothing.
*/
template<typename StreamFactory>
void
testAlpn(StreamFactory make_stream, bool alpn_supported)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    client_ctx.set_alpn({"h2", "http/1.1"});
    auto server_ctx = make_server_context();
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.set_alpn({"h2", "http/1.1"});

    auto client       = make_stream(m1, client_ctx);
    auto server       = make_stream(m2, server_ctx);
    using stream_type = std::remove_reference_t<decltype(server)>;

    std::error_code cec, sec;
    auto hc = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(stream_type::client);
        cec       = ec;
    };
    auto hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(stream_type::server);
        sec       = ec;
    };
    capy::run_async(ioc.get_executor())(hc());
    capy::run_async(ioc.get_executor())(hs());
    ioc.run();

    if (alpn_supported)
    {
        BOOST_TEST(!cec);
        BOOST_TEST(!sec);
        BOOST_TEST(client.alpn_protocol() == "h2");
        BOOST_TEST(server.alpn_protocol() == "h2");
    }
    else
    {
        // Fail-closed: offering ALPN a build cannot honor must not
        // silently proceed.
        BOOST_TEST(
            cec == std::errc::function_not_supported ||
            sec == std::errc::function_not_supported);
    }

    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** ALPN with no common protocol fails the handshake (RFC 7301 §3.2).

    Client offers `{"h2"}`, server offers `{"http/1.1"}`. A server that
    supports ALPN but shares no protocol with the client must abort with a
    fatal `no_application_protocol` alert, so both peers see the handshake
    fail.

    @param alpn_supported Whether the backend build can negotiate ALPN. When
        false (e.g. WolfSSL without `HAVE_ALPN`) offering ALPN already fails
        closed regardless of overlap — that path is covered by @ref testAlpn —
        so this test only runs when ALPN is available.
*/
template<typename StreamFactory>
void
testAlpnNoOverlap(StreamFactory make_stream, bool alpn_supported)
{
    if (!alpn_supported)
        return;

    io_context ioc;
    auto client_ctx = make_client_context();
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    client_ctx.set_alpn({"h2"});
    auto server_ctx = make_server_context();
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    server_ctx.set_alpn({"http/1.1"});
    run_tls_test_fail(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Test the certificate verification callback (portable contract).

    Exercises the guarantees that hold on every backend that supports the
    callback: it is consulted when the backend's built-in verification
    fails, its return value determines the outcome, and it can inspect the
    certificate's DER portably.

      1. Override accept: an untrusted CA would normally fail, but a
         callback returning true completes the handshake, and the
         callback observes preverified == false with a usable native
         handle and DER bytes.
      2. Decline: an untrusted CA with a callback returning false leaves
         the handshake failed.

    @param callback_supported Whether this backend build can honor a
        verify callback. Pass `false` for builds that cannot invoke the
        callback on a successful handshake (e.g. WolfSSL without
        WOLFSSL_ALWAYS_VERIFY_CB): installing a callback must then fail
        the handshake with `std::errc::function_not_supported` rather than
        silently ignore it (which would let a tightening callback fail
        open).
*/
template<typename StreamFactory>
void
testVerifyCallback(StreamFactory make_stream, bool callback_supported = true)
{
    if (!callback_supported)
    {
        // Fail-closed: a context carrying a verify_callback must fail the
        // handshake with a clear error, never silently accept.
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            client_ctx.set_verify_callback(
                [](bool preverified, verify_context&) -> bool {
                    return preverified;
                });
            auto server_ctx = make_server_context();
            std::error_code client_ec;
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream,
                &client_ec);
            BOOST_TEST(client_ec == std::errc::function_not_supported);
        }

        // A retried handshake on the same stream must stay fail-closed. The
        // fail-closed path returns before any I/O, so this needs no peer;
        // it is a regression guard against leaving the native session
        // non-null (which would let the retry bypass the check).
        {
            io_context ioc;
            auto [m1, m2] = corosio::test::make_mocket_pair(ioc);
            (void)m2;
            auto client_ctx = make_client_context();
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            client_ctx.set_verify_callback(
                [](bool preverified, verify_context&) -> bool {
                    return preverified;
                });
            auto client       = make_stream(m1, client_ctx);
            using stream_type = std::remove_reference_t<decltype(client)>;

            std::error_code ec1;
            std::error_code ec2;
            auto attempt = [&](std::error_code& out) -> capy::task<> {
                auto [ec] = co_await client.handshake(stream_type::client);
                out = ec;
            };
            capy::run_async(ioc.get_executor())(attempt(ec1));
            ioc.run();
            ioc.restart();
            capy::run_async(ioc.get_executor())(attempt(ec2));
            ioc.run();

            BOOST_TEST(ec1 == std::errc::function_not_supported);
            BOOST_TEST(ec2 == std::errc::function_not_supported);

            m1.close(); // NOLINT(bugprone-unused-return-value)
        }
        return;
    }

    // 1. Override accept: untrusted CA (preverified == false) but callback
    //    returns true -> handshake succeeds. Without the callback this
    //    configuration fails, so success proves the callback is consulted
    //    and its return value controls the result.
    {
        io_context ioc;
        bool saw_unverified = false;

        auto client_ctx = make_wrong_ca_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_callback(
            [&saw_unverified](bool preverified, verify_context& vc) -> bool {
                if (!preverified)
                {
                    saw_unverified = true;
                    BOOST_TEST(vc.native_handle() != nullptr);
                    // Portable certificate access: the callback must see the
                    // DER bytes of the cert under verification on both
                    // backends. A DER certificate is an ASN.1 SEQUENCE, so
                    // the first byte is 0x30.
                    auto der = vc.certificate();
                    BOOST_TEST(!der.empty());
                    if (!der.empty())
                        BOOST_TEST(der[0] == 0x30);
                }
                return true;
            });

        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(saw_unverified);
    }

    // 2. Decline: untrusted CA and callback returns false -> stays failed.
    {
        io_context ioc;
        auto client_ctx = make_wrong_ca_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_callback(
            [](bool, verify_context&) -> bool { return false; });

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that the verify callback also runs on successful verification.

    Backends that support the full callback contract (OpenSSL always;
    WolfSSL with WOLFSSL_ALWAYS_VERIFY_CB) invoke it for a certificate that
    passed the built-in checks, which lets a callback reject an otherwise
    valid certificate (e.g. for pinning) and inspect it on the success
    path. Only call this for backends where the callback is fully
    supported.
*/
template<typename StreamFactory>
void
testVerifyCallbackOnSuccess(StreamFactory make_stream)
{
    // Invoked on success: trusted CA + pass-through callback succeeds, and
    // the certificate is inspectable on the success path.
    {
        io_context ioc;
        bool invoked  = false;
        bool saw_cert = false;

        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_callback(
            [&](bool preverified, verify_context& vc) -> bool {
                invoked = true;
                if (preverified && !vc.certificate().empty() &&
                    vc.certificate()[0] == 0x30)
                    saw_cert = true;
                return preverified;
            });

        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(invoked);
        BOOST_TEST(saw_cert);
    }

    // Reject a valid cert: trusted CA but callback returns false -> fail.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_callback(
            [](bool, verify_context&) -> bool { return false; });

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Content-based pinning: reject unless the leaf DER matches an expected
    // pin. A deliberately-wrong pin must fail the handshake even though the
    // chain is otherwise valid.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        client_ctx.set_verify_callback(
            [](bool preverified, verify_context& vc) -> bool {
                if (!preverified)
                    return false;
                auto der = vc.certificate();
                return der.size() == 1 && der[0] == 0xFF; // never matches
            });

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
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
    std::stop_source failsafe_stop;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await client.handshake(stream_type::client);
        client_ec   = ec;
        client_done = true;
        if (server_done)
            failsafe_stop.request_stop();
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await server.handshake(stream_type::server);
        server_ec   = ec;
        server_done = true;
        if (client_done)
            failsafe_stop.request_stop();
    };
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(std::chrono::seconds(5));
        if (!ec)
        {
            failsafe_hit = true;
            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
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
