//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// WolfSSL has no equivalent to OpenSSL's anonymous cipher string
// "aNULL:eNULL:@SECLEVEL=0", so context_mode::anon is skipped here;
// shared_cert and separate_cert modes exercise both backends the
// same way.

// Test that header file is self-contained.
#include <boost/corosio/wolfssl_stream.hpp>

#include "tls_stream_tests.hpp"

#ifdef BOOST_COROSIO_HAS_WOLFSSL

#include <filesystem>
#include <fstream>
#include <random>

namespace boost::corosio {

// Callable wrapper for passing to test helper templates
struct wolfssl_stream_factory
{
    auto operator()(tcp_socket& s, tls_context const& ctx) const
    {
        return wolfssl_stream(&s, ctx);
    }

    auto operator()(corosio::test::mocket& s, tls_context const& ctx) const
    {
        return wolfssl_stream(&s, ctx);
    }

    auto operator()(test::gated_stream& s, tls_context const& ctx) const
    {
        return wolfssl_stream(&s, ctx);
    }

    auto operator()(test::partial_error_stream& s, tls_context const& ctx) const
    {
        return wolfssl_stream(&s, ctx);
    }
};

struct wolfssl_stream_test
{
    static constexpr wolfssl_stream_factory make_stream{};

    // Context modes supported by WolfSSL (no anon ciphers)
    static constexpr std::array<test::context_mode, 2> cert_modes = {
        test::context_mode::shared_cert, test::context_mode::separate_cert};

    void testName()
    {
        using namespace test;

        io_context ioc;
        auto ctx = make_client_context();
        tcp_socket sock(ioc);
        wolfssl_stream stream(&sock, ctx);

        BOOST_TEST(stream.name() == "wolfssl");
    }

    /** Exercise next_layer() accessors (const and non-const). */
    void testNextLayer()
    {
        using namespace test;

        io_context ioc;
        auto ctx = make_client_context();
        tcp_socket sock(ioc);
        wolfssl_stream stream(&sock, ctx);

        capy::any_stream& mutable_next = stream.next_layer();
        (void)mutable_next;

        wolfssl_stream const& cref = stream;
        capy::any_stream const& const_next = cref.next_layer();
        (void)const_next;

        BOOST_TEST(&mutable_next == &const_next);
    }

    /** Test that WolfSSL errors carry the WolfSSL category.

        Errors from wolfSSL_get_error must render readable messages, not
        garbage produced by treating the code as an errno value.
    */
    void testErrorCategory()
    {
        using namespace test;

        BOOST_TEST(wolfssl_category().name() ==
            std::string_view("corosio.wolfssl"));

        // End-to-end: a certificate-validation failure surfaces an error
        // in the WolfSSL category with a non-empty, decoded message.
        {
            io_context ioc;
            auto client_ctx = make_untrusted_ca_client_context();
            auto server_ctx = make_server_context();
            std::error_code client_ec;
            run_tls_test_fail(ioc, client_ctx, server_ctx, make_stream,
                make_stream, &client_ec);
            BOOST_TEST(client_ec);
            BOOST_TEST(client_ec.category() == wolfssl_category());
            BOOST_TEST(!client_ec.message().empty());
        }
    }

    /** Test that add_verify_path() loads CAs from a directory.

        WolfSSL loads every certificate file in the directory, so no
        hash-based naming is required. A client that trusts the CA only
        through add_verify_path() must be able to verify the server.
    */
    void testAddVerifyPath()
    {
        using namespace test;

        // A per-process-unique directory: the cxstd variants of this test
        // run concurrently, and a shared path would let one process remove
        // the CA file while another loads it, emptying the trust store and
        // failing the handshake as ASN_NO_SIGNER_E.
        auto dir = std::filesystem::temp_directory_path() /
            ("corosio_wolfssl_capath_" +
                std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);
        auto ca_file = dir / "test_ca.pem";
        {
            std::ofstream out(ca_file, std::ios::binary);
            out << ca_cert_pem;
        }

        {
            io_context ioc;
            tls_context client_ctx;
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            client_ctx.add_verify_path(dir.string());
            // NOLINTNEXTLINE(bugprone-unused-return-value)
            client_ctx.set_verify_mode(tls_verify_mode::peer);

            auto server_ctx = make_server_context();
            run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        }

        std::filesystem::remove_all(dir);
    }

    void run()
    {
        test::testHandshakeFuse(make_stream);
        test::testReadWriteFuse(make_stream);
        test::testShutdownFuse(make_stream);
        // Skip anon mode: anonymous cipher string "aNULL:eNULL:@SECLEVEL=0"
        // is OpenSSL-specific and not supported by WolfSSL.
        test::testSuccessCases(make_stream, cert_modes);
        test::testFailureCases(make_stream);
        test::testTlsShutdown(make_stream, cert_modes);
        test::testStreamTruncated(make_stream, cert_modes);
        test::testStopTokenCancellation(make_stream);
        test::testShutdownCancel(make_stream);
        test::testSocketErrorPropagation(make_stream);
        test::testCertificateValidation(make_stream);
        test::testSni(make_stream);
        test::testSniCallback(make_stream);
        test::testHostnamePersistence(make_stream);
        test::testHostnameRedirect(make_stream);
        test::testHostnameClear(make_stream);
        test::testFullDuplex(make_stream);
        test::testFullDuplexBulk(make_stream);
        test::testRecordBoundaryTransfer(make_stream);
        test::testShutdownOverRead(make_stream);
        test::testShutdownSimultaneousClose(make_stream);
        test::testPartialReadWithError(make_stream);
        test::testCancelParkedReader(make_stream);
        test::testFullDuplexMtStrand(make_stream);
        test::testDeferredFlushError(make_stream);
        test::testTlsLifecycleEdges(make_stream);
        test::testShutdownTruncation(make_stream);
        test::testHostnameRetryAfterFailure(make_stream);
        // IP-literal matching is build-gated (WOLFSSL_IP_ALT_NAME);
        // when absent, an IP-literal hostname fails closed.
        test::testHostnameIpLiteral(
            make_stream, wolfssl_supports_ip_alt_name());
        test::testAlpnAccessorEmpty(make_stream);
        // Whether the linked WolfSSL can honor a verify callback on success
        // (WOLFSSL_ALWAYS_VERIFY_CB) is a build-time property, queried here
        // at runtime so the test needs no WolfSSL headers.
        if (wolfssl_supports_verify_callback())
        {
            // Capable build: full OpenSSL-parity semantics (override,
            // decline, inspect, tighten).
            test::testVerifyCallback(make_stream, /*callback_supported=*/true);
            test::testVerifyCallbackOnSuccess(make_stream);
        }
        else
        {
            // Minimal build: the callback cannot fire on success, so corosio
            // fails closed rather than let a tightening callback fail open.
            test::testVerifyCallback(make_stream, /*callback_supported=*/false);
        }
        // ALPN is likewise build-gated (HAVE_ALPN); when absent, offering
        // protocols fails closed instead of negotiating nothing silently.
        test::testAlpn(make_stream, wolfssl_supports_alpn());
        test::testAlpnNoOverlap(make_stream, wolfssl_supports_alpn());
        test::testProtocolVersion(make_stream);
        test::testCiphersuitesTls13(
            make_stream, "TLS13-AES128-GCM-SHA256",
            "TLS13-AES256-GCM-SHA384");
        test::testPkcs12(make_stream);
        test::testPkcs12Chain(make_stream);
        test::testCertificateChain(make_stream);
        test::testDefaultVerifyPaths(make_stream);
        test::testCrlRevocation(make_stream, wolfssl_supports_crl());
        test::testMtls(make_stream);
        test::testMoveSemantics(make_stream);
        test::testAbruptClose(make_stream);
        // Encrypted-key decryption is a compile-time wolfSSL feature;
        // require only that the load path runs and fails cleanly.
        test::testEncryptedKey(make_stream, /*expect_success=*/false);
        test::testInvalidContextHandshake(make_stream);

        test::testReset(make_stream, cert_modes);
        test::testResetViaHandshake(make_stream, cert_modes);
        test::testResetFuse(make_stream);

        testErrorCategory();
        testAddVerifyPath();
        testName();
        testNextLayer();
    }
};

TEST_SUITE(wolfssl_stream_test, "boost.corosio.wolfssl_stream");

} // namespace boost::corosio

#endif
