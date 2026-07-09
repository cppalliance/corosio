//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Examples demonstrating tls_context API usage patterns.
//
// The configuration setters return a std::error_code rather than throwing.
// These examples use the small must() helper to turn a failure into an
// exception where that keeps the example terse; load_checked() and
// load_mixed() below show the explicit error_code style instead.

#include <boost/corosio/tls_context.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

using namespace boost::corosio;

// Throw std::system_error if an operation reported a failure.
static void
must(std::error_code ec)
{
    if (ec)
        throw std::system_error(ec);
}

//
// HTTPS Client Context
//

// Basic HTTPS client that trusts system CAs
tls_context make_https_client()
{
    tls_context ctx;

    // Use system trust store for public websites
    must(ctx.set_default_verify_paths());

    // Verify the server certificate
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    // Set the hostname for SNI and certificate verification
    ctx.set_hostname( "api.example.com" );

    return ctx;
}

// HTTPS client with pinned CA (don't trust system store)
tls_context make_pinned_ca_client( std::string_view ca_pem )
{
    tls_context ctx;

    // Only trust this specific CA
    must(ctx.add_certificate_authority( ca_pem ));

    must(ctx.set_verify_mode( tls_verify_mode::peer ));
    ctx.set_hostname( "internal.example.com" );

    return ctx;
}

// HTTP/2 client with ALPN
tls_context make_http2_client()
{
    tls_context ctx;

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    // Prefer HTTP/2, fall back to HTTP/1.1
    must(ctx.set_alpn( { "h2", "http/1.1" } ));

    return ctx;
}

//
// TLS Server Context
//

// Basic TLS server (no client verification)
tls_context make_basic_server()
{
    tls_context ctx;

    // Load certificate chain and private key
    must(ctx.use_certificate_chain_file( "server-fullchain.pem" ));
    must(ctx.use_private_key_file( "server.key", tls_file_format::pem ));

    // Don't verify clients (no mTLS)
    must(ctx.set_verify_mode( tls_verify_mode::none ));

    return ctx;
}

// mTLS server (requires client certificates)
tls_context make_mtls_server()
{
    tls_context ctx;

    // Server credentials
    must(ctx.use_certificate_chain_file( "server-fullchain.pem" ));
    must(ctx.use_private_key_file( "server.key", tls_file_format::pem ));

    // Trust this CA for client certificates
    must(ctx.load_verify_file( "client-ca.crt" ));

    // Require clients to present a valid certificate
    must(ctx.set_verify_mode( tls_verify_mode::require_peer ));

    return ctx;
}

// Server with PKCS#12 credentials
tls_context make_server_from_pfx()
{
    tls_context ctx;

    // Load all credentials from a single file
    must(ctx.use_pkcs12_file( "server.pfx", "bundle-password" ));

    must(ctx.set_verify_mode( tls_verify_mode::none ));

    return ctx;
}

// Server with encrypted private key
tls_context make_server_encrypted_key()
{
    tls_context ctx;

    // Set password callback before loading encrypted key
    ctx.set_password_callback(
        []( std::size_t max_len, tls_password_purpose purpose )
        {
            (void)max_len;
            (void)purpose;
            // Read from environment or secret manager
            char const* pw = std::getenv( "TLS_KEY_PASSWORD" );
            return std::string( pw ? pw : "" );
        });

    must(ctx.use_certificate_chain_file( "server.crt" ));
    must(ctx.use_private_key_file(
        "server-encrypted.key", tls_file_format::pem ));

    return ctx;
}

//
// mTLS Client Context
//

// Client with client certificate for mTLS
tls_context make_mtls_client()
{
    tls_context ctx;

    // Client credentials for mTLS
    must(ctx.use_certificate_file( "client.crt", tls_file_format::pem ));
    must(ctx.use_private_key_file( "client.key", tls_file_format::pem ));

    // Trust specific CA for server verification
    must(ctx.load_verify_file( "server-ca.crt" ));
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

//
// Protocol Version Configuration
//

// TLS 1.3 only
tls_context make_tls13_only()
{
    tls_context ctx;

    must(ctx.set_min_protocol_version( tls_version::tls_1_3 ));
    must(ctx.set_max_protocol_version( tls_version::tls_1_3 ));

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

// Allow TLS 1.2+ (default behavior made explicit)
tls_context make_tls12_plus()
{
    tls_context ctx;

    must(ctx.set_min_protocol_version( tls_version::tls_1_2 ));
    // No max = allow newest

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

//
// Cipher Suite Configuration
//

// High-security cipher configuration
tls_context make_high_security()
{
    tls_context ctx;

    // Only ECDHE key exchange with AESGCM or ChaCha20
    must(ctx.set_ciphersuites( "ECDHE+AESGCM:ECDHE+CHACHA20" ));

    // TLS 1.3 only
    must(ctx.set_min_protocol_version( tls_version::tls_1_3 ));

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

//
// Revocation Checking
//

// Client with CRL checking
tls_context make_client_with_crl( std::string_view crl_path )
{
    tls_context ctx;

    must(ctx.set_default_verify_paths());
    must(ctx.add_crl_file( crl_path ));

    // Fail if certificate is revoked, allow if status unknown
    ctx.set_revocation_policy( tls_revocation_policy::soft_fail );

    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

// Strict revocation checking
tls_context make_hardened_client()
{
    tls_context ctx;

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    // Fail if revocation status cannot be determined
    ctx.set_revocation_policy( tls_revocation_policy::hard_fail );

    return ctx;
}

//
// Custom Verification
//

// Client that pins a specific certificate via a verification callback.
tls_context make_client_custom_verify( std::span<unsigned char const> pin )
{
    tls_context ctx;

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    must(ctx.set_verify_callback(
        [pin]( bool preverified, verify_context& verify_ctx ) -> bool
        {
            // Require the chain to verify normally first.
            if( !preverified )
                return false;

            // Then pin: accept only if the certificate's DER matches. The
            // DER bytes are available portably across backends via
            // certificate(); the raw X509_STORE_CTX* is also reachable
            // through verify_ctx.native_handle() for backend-specific use.
            auto der = verify_ctx.certificate();
            return der.size() == pin.size() &&
                std::equal( der.begin(), der.end(), pin.begin() );
        }));

    return ctx;
}

// Verify depth limit
tls_context make_client_limited_depth()
{
    tls_context ctx;

    must(ctx.set_default_verify_paths());
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    // Allow at most 2 intermediate certificates
    must(ctx.set_verify_depth( 2 ));

    return ctx;
}

//
// Loading from Memory
//

// Load all credentials from memory buffers
tls_context make_from_memory(
    std::string_view cert_pem,
    std::string_view key_pem,
    std::string_view ca_pem )
{
    tls_context ctx;

    // From vault/secret manager
    must(ctx.use_certificate_chain( cert_pem ));
    must(ctx.use_private_key( key_pem, tls_file_format::pem ));
    must(ctx.add_certificate_authority( ca_pem ));

    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

// Load PKCS#12 from memory
tls_context make_from_pkcs12_memory(
    std::string_view pkcs12_data,
    std::string_view passphrase )
{
    tls_context ctx;

    must(ctx.use_pkcs12( pkcs12_data, passphrase ));
    must(ctx.set_verify_mode( tls_verify_mode::peer ));

    return ctx;
}

//
// DER Format
//

// Load DER-encoded certificate and key
tls_context make_from_der()
{
    tls_context ctx;

    must(ctx.use_certificate_file( "server.der", tls_file_format::der ));
    must(ctx.use_private_key_file( "server.key.der", tls_file_format::der ));

    return ctx;
}

//
// Shared Context
//

// Demonstrate shared ownership
void demonstrate_sharing()
{
    // Create a context
    tls_context original;
    must(original.set_default_verify_paths());
    must(original.set_verify_mode( tls_verify_mode::peer ));

    // Share via copy - both point to same underlying state
    tls_context copy1 = original;
    tls_context copy2 = original;
    (void)copy1;
    (void)copy2;

    // Changes to copy1 affect copy2 and original
    // (they all share the same impl)

    // Move transfers ownership
    tls_context moved = std::move( original );
    (void)moved;
    // original is now empty
}

//
// Error Handling Patterns
//

// Throw on error (simple code, let exceptions propagate)
void load_throwing()
{
    tls_context ctx;

    must(ctx.use_certificate_chain_file( "cert.pem" ));  // throws on error
    must(ctx.use_private_key_file( "key.pem", tls_file_format::pem ));
    must(ctx.set_default_verify_paths());
}

// Check errors explicitly
bool load_checked( tls_context& ctx, std::string& error_msg )
{
    if( auto ec = ctx.use_certificate_chain_file( "cert.pem" ); ec )
    {
        error_msg = "Certificate: " + ec.message();
        return false;
    }

    if( auto ec = ctx.use_private_key_file( "key.pem", tls_file_format::pem ); ec )
    {
        error_msg = "Key: " + ec.message();
        return false;
    }

    if( auto ec = ctx.set_default_verify_paths(); ec )
    {
        error_msg = "CA store: " + ec.message();
        return false;
    }

    return true;
}

// Mixed approach - throw for programmer errors, check for runtime errors
void load_mixed()
{
    tls_context ctx;

    // File loading might fail at runtime
    if( auto ec = ctx.use_certificate_chain_file( "cert.pem" ); ec )
    {
        // Handle missing file gracefully
        std::cerr << "Certificate not found: " << ec.message() << "\n";
        return;
    }

    // Protocol settings won't fail if arguments are valid
    must(ctx.set_min_protocol_version( tls_version::tls_1_2 ));
    must(ctx.set_verify_mode( tls_verify_mode::peer ));
}

//
// Main
//

int main()
{
    // These examples demonstrate API ergonomics
    try
    {
        auto https  = make_https_client();
        auto server = make_basic_server();
        auto mtls   = make_mtls_server();
        (void)https;
        (void)server;
        (void)mtls;
    }
    catch( std::exception const& e )
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
