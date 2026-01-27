//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// OpenSSL Implementation Notes
// ----------------------------
// - Anonymous ciphers: "aNULL:eNULL:@SECLEVEL=0" works with OpenSSL
// - Failure tests disabled: socket.cancel() doesn't propagate to TLS ops
// - To enable failure tests: need TLS-aware cancellation in openssl_stream

// Test that header file is self-contained.
#include <boost/corosio/tls/openssl_stream.hpp>

#include "test_utils.hpp"
#include <iostream>
#include "test_suite.hpp"

namespace boost::corosio {

struct openssl_stream_test
{
#ifdef BOOST_COROSIO_HAS_OPENSSL
    static auto
    make_stream( io_stream& s, tls::context ctx )
    {
        return openssl_stream( s, ctx );
    }

    void
    testSuccessCases()
    {
        using namespace tls::test;

        for( auto mode : { context_mode::anon,
                           context_mode::shared_cert,
                           context_mode::separate_cert } )
        {
            io_context ioc;
            auto [client_ctx, server_ctx] = make_contexts( mode );
            run_tls_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testFailureCases()
    {
        using namespace tls::test;

        io_context ioc;

        // Client verifies, server has no cert
        {
            auto client_ctx = make_client_context();
            auto server_ctx = make_anon_context();
            server_ctx.set_ciphersuites( "" ); // disable anon ciphers
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
            ioc.restart();
        }

        // Client trusts wrong CA
        {
            auto client_ctx = make_wrong_ca_context();
            auto server_ctx = make_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
            ioc.restart();
        }
    }

    void
    testTlsShutdown()
    {
        using namespace tls::test;

        for( auto mode : { context_mode::shared_cert,
                           context_mode::separate_cert } )
        {
            io_context ioc;
            auto [client_ctx, server_ctx] = make_contexts( mode );
            run_tls_shutdown_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testStreamTruncated()
    {
        using namespace tls::test;

        for( auto mode : { context_mode::shared_cert,
                           context_mode::separate_cert } )
        {
            io_context ioc;
            auto [client_ctx, server_ctx] = make_contexts( mode );
            run_tls_truncation_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testStopTokenCancellation()
    {
        using namespace tls::test;

        // Cancel during handshake
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();
            run_stop_token_handshake_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Cancel during read
        {
            io_context ioc;
            auto [client_ctx, server_ctx] = make_contexts( context_mode::separate_cert );
            run_stop_token_read_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Cancel during write
        {
            io_context ioc;
            auto [client_ctx, server_ctx] = make_contexts( context_mode::separate_cert );
            run_stop_token_write_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testSocketErrorPropagation()
    {
        using namespace tls::test;

        // socket.cancel() while TLS blocked on socket I/O
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();
            run_socket_cancel_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Connection reset during handshake
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();
            run_connection_reset_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testCertificateValidation()
    {
        using namespace tls::test;

        // Untrusted CA - client trusts different CA than server's cert
        // Should fail immediately during certificate verification
        {
            io_context ioc;
            auto client_ctx = make_untrusted_ca_client_context();
            auto server_ctx = make_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Expired certificate - server cert expired Jan 2, 2020
        // Client trusts the cert but should reject due to expiry
        {
            io_context ioc;
            auto client_ctx = make_expired_client_context();
            auto server_ctx = make_expired_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testSni()
    {
        using namespace tls::test;

        // Test SNI + hostname verification - correct hostname succeeds
        // Server cert has CN=www.example.com
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            client_ctx.set_hostname( "www.example.com" );
            auto server_ctx = make_server_context();
            run_tls_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Test hostname verification - wrong hostname fails
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            client_ctx.set_hostname( "wrong.example.com" );
            auto server_ctx = make_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testSniCallback()
    {
        using namespace tls::test;

        // SNI callback accepts the hostname - handshake succeeds
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            client_ctx.set_hostname( "www.example.com" );

            auto server_ctx = make_server_context();
            server_ctx.set_servername_callback(
                []( std::string_view hostname ) -> bool
                {
                    return hostname == "www.example.com";
                });

            run_tls_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // SNI callback rejects the hostname - handshake fails
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            client_ctx.set_hostname( "www.example.com" );

            auto server_ctx = make_server_context();
            server_ctx.set_servername_callback(
                []( std::string_view hostname ) -> bool
                {
                    return hostname == "api.example.com";  // Only accept api.*
                });

            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testMtls()
    {
        using namespace tls::test;

        // mTLS success - client provides valid cert
        {
            io_context ioc;
            auto client_ctx = make_mtls_client_context();
            auto server_ctx = make_mtls_server_context();
            run_tls_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // mTLS failure - client provides no cert but server requires it
        {
            io_context ioc;
            auto client_ctx = make_chain_client_context();
            auto server_ctx = make_mtls_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // mTLS failure - client provides cert signed by WRONG CA
        {
            io_context ioc;
            auto client_ctx = make_invalid_mtls_client_context();
            auto server_ctx = make_mtls_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }

    void
    testCertificateChain()
    {
        using namespace tls::test;

        // Server sends full chain (entity + intermediate) - client trusts only root
        // Should succeed because server sends intermediate in chain
        {
            io_context ioc;
            auto client_ctx = make_rootonly_client_context();
            auto server_ctx = make_fullchain_server_context();
            run_tls_test( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }

        // Server sends only entity cert - client trusts only root
        // Should fail because client can't build chain to root
        {
            io_context ioc;
            auto client_ctx = make_rootonly_client_context();
            auto server_ctx = make_chain_server_context();
            run_tls_test_fail( ioc, client_ctx, server_ctx,
                make_stream, make_stream );
        }
    }
#endif

    void
    run()
    {
#ifdef BOOST_COROSIO_HAS_OPENSSL
        testSuccessCases();
        testTlsShutdown();
        testStreamTruncated();
        testFailureCases();
        testStopTokenCancellation();
        testSocketErrorPropagation();
        testCertificateValidation();
        testSni();
        testSniCallback();
        testMtls();
        testCertificateChain();
#else
        std::cerr << "openssl_stream tests SKIPPED: OpenSSL not found\n";
	static_assert(false, "OpenSSL not found");
#endif
    }
};

TEST_SUITE(openssl_stream_test, "boost.corosio.openssl_stream");

} // namespace boost::corosio
