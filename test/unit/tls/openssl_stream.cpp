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
#include "test_suite.hpp"

namespace boost {
namespace corosio {

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
#endif

    void
    run()
    {
#ifdef BOOST_COROSIO_HAS_OPENSSL
        testSuccessCases();
        testTlsShutdown();
        testStreamTruncated();
        // Failure tests disabled: socket cancellation doesn't propagate to
        // TLS handshake operations, causing hangs when one side fails.
        // testFailureCases();
#endif
    }
};

TEST_SUITE(openssl_stream_test, "boost.corosio.openssl_stream");

} // namespace corosio
} // namespace boost
