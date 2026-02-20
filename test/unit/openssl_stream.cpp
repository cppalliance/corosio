//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/openssl_stream.hpp>

#include "tls_stream_tests.hpp"

#ifdef BOOST_COROSIO_HAS_OPENSSL

namespace boost::corosio {

// Callable wrapper for passing to test helper templates
struct openssl_stream_factory
{
    auto operator()(tcp_socket& s, tls_context ctx) const
    {
        return openssl_stream(&s, ctx);
    }

    auto operator()(corosio::test::mocket& s, tls_context ctx) const
    {
        return openssl_stream(&s, ctx);
    }
};

struct openssl_stream_test
{
    static constexpr openssl_stream_factory make_stream{};

    // Context modes supported by OpenSSL (includes anon ciphers)
    static constexpr std::array<test::context_mode, 3> all_modes = {
        test::context_mode::anon, test::context_mode::shared_cert,
        test::context_mode::separate_cert};

    static constexpr std::array<test::context_mode, 2> cert_modes = {
        test::context_mode::shared_cert, test::context_mode::separate_cert};

    void testName()
    {
        using namespace test;

        io_context ioc;
        auto ctx = make_anon_context();
        tcp_socket sock(ioc);
        openssl_stream stream(&sock, ctx);

        BOOST_TEST(stream.name() == "openssl");
    }

    /** Test certificate chain validation (OpenSSL-specific).

        OpenSSL supports sending full certificate chains via
        use_certificate_chain() and add_extra_chain_cert().
    */
    void testCertificateChain()
    {
        using namespace test;

        // Server sends full chain
        {
            io_context ioc;
            auto client_ctx = make_rootonly_client_context();
            auto server_ctx = make_fullchain_server_context();
            run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        }

        // Server sends only entity cert (fails)
        {
            io_context ioc;
            auto client_ctx = make_rootonly_client_context();
            auto server_ctx = make_chain_server_context();
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }
    }

    void run()
    {
        test::testHandshakeFuse(make_stream);
        test::testReadWriteFuse(make_stream);
        test::testShutdownFuse(make_stream);
        test::testSuccessCases(make_stream, all_modes);
        test::testFailureCases(make_stream);
        test::testTlsShutdown(make_stream, cert_modes);
        test::testStreamTruncated(make_stream, cert_modes);
        test::testStopTokenCancellation(make_stream);
        test::testSocketErrorPropagation(make_stream);
        test::testCertificateValidation(make_stream);
        test::testSni(make_stream);
        test::testSniCallback(make_stream);
        test::testMtls(make_stream);

        test::testReset(make_stream, cert_modes);
        test::testResetViaHandshake(make_stream, cert_modes);
        test::testResetFuse(make_stream);

        testCertificateChain();
        testName();
    }
};

TEST_SUITE(openssl_stream_test, "boost.corosio.openssl_stream");

} // namespace boost::corosio

#endif
