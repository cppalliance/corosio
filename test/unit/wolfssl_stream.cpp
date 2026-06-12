//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// WolfSSL Implementation Notes
// ----------------------------
// - Anonymous ciphers: "aNULL:eNULL:@SECLEVEL=0" is OpenSSL syntax, doesn't work
// - WolfSSL anon ciphers require compile-time flags and different cipher string
// - context_mode::anon skipped; shared_cert and separate_cert modes work

// Test that header file is self-contained.
#include <boost/corosio/wolfssl_stream.hpp>

#include "tls_stream_tests.hpp"

#ifdef BOOST_COROSIO_HAS_WOLFSSL

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

    /** Test certificate chain validation (WolfSSL-specific).

        WolfSSL has limited certificate chain support compared to OpenSSL.
        wolfSSL_CTX_add_extra_chain_cert doesn't properly send intermediates
        during handshake, so fullchain tests are disabled.
    */
    void testCertificateChain()
    {
        using namespace test;

        // Basic chain test: client trusts both CAs
        {
            io_context ioc;
            auto client_ctx = make_chain_client_context();
            auto server_ctx = make_chain_server_context();
            run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        }

        // Server sends only entity cert - client trusts only root (fails)
        {
            io_context ioc;
            auto client_ctx = make_rootonly_client_context();
            auto server_ctx = make_chain_server_context();
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }

        // Note: Fullchain test disabled for WolfSSL due to
        // wolfSSL_CTX_add_extra_chain_cert not properly sending
        // intermediates during handshake.
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
        test::testSocketErrorPropagation(make_stream);
        test::testCertificateValidation(make_stream);
        test::testSni(make_stream);
        test::testSniCallback(make_stream);
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

        testCertificateChain();
        testName();
        testNextLayer();
    }
};

TEST_SUITE(wolfssl_stream_test, "boost.corosio.wolfssl_stream");

} // namespace boost::corosio

#endif
