//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Examples demonstrating tls::context API usage patterns.
// These examples are not meant to compile; they validate API ergonomics.

#include <boost/corosio/tls/context.hpp>

namespace tls = boost::corosio::tls;

//------------------------------------------------------------------------------
//
// HTTPS Client Context
//
//------------------------------------------------------------------------------

// Basic HTTPS client that trusts system CAs
tls::context make_https_client()
{
    tls::context ctx;
    
    // Use system trust store for public websites
    ctx.set_default_verify_paths().value();
    
    // Verify the server certificate
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Set the hostname for SNI and certificate verification
    ctx.set_hostname( "api.example.com" );
    
    return ctx;
}

// HTTPS client with pinned CA (don't trust system store)
tls::context make_pinned_ca_client( std::string_view ca_pem )
{
    tls::context ctx;
    
    // Only trust this specific CA
    ctx.add_certificate_authority( ca_pem ).value();
    
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    ctx.set_hostname( "internal.example.com" );
    
    return ctx;
}

// HTTP/2 client with ALPN
tls::context make_http2_client()
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Prefer HTTP/2, fall back to HTTP/1.1
    ctx.set_alpn( { "h2", "http/1.1" } ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// TLS Server Context
//
//------------------------------------------------------------------------------

// Basic TLS server (no client verification)
tls::context make_basic_server()
{
    tls::context ctx;
    
    // Load certificate chain and private key
    ctx.use_certificate_chain_file( "server-fullchain.pem" ).value();
    ctx.use_private_key_file( "server.key", tls::file_format::pem ).value();
    
    // Don't verify clients (no mTLS)
    ctx.set_verify_mode( tls::verify_mode::none ).value();
    
    return ctx;
}

// mTLS server (requires client certificates)
tls::context make_mtls_server()
{
    tls::context ctx;
    
    // Server credentials
    ctx.use_certificate_chain_file( "server-fullchain.pem" ).value();
    ctx.use_private_key_file( "server.key", tls::file_format::pem ).value();
    
    // Trust this CA for client certificates
    ctx.load_verify_file( "client-ca.crt" ).value();
    
    // Require clients to present a valid certificate
    ctx.set_verify_mode( tls::verify_mode::require_peer ).value();
    
    return ctx;
}

// Server with PKCS#12 credentials
tls::context make_server_from_pfx()
{
    tls::context ctx;
    
    // Load all credentials from a single file
    ctx.use_pkcs12_file( "server.pfx", "bundle-password" ).value();
    
    ctx.set_verify_mode( tls::verify_mode::none ).value();
    
    return ctx;
}

// Server with encrypted private key
tls::context make_server_encrypted_key()
{
    tls::context ctx;
    
    // Set password callback before loading encrypted key
    ctx.set_password_callback(
        []( std::size_t max_len, tls::password_purpose purpose )
        {
            // Read from environment or secret manager
            return std::string( std::getenv( "TLS_KEY_PASSWORD" ) );
        });
    
    ctx.use_certificate_chain_file( "server.crt" ).value();
    ctx.use_private_key_file( "server-encrypted.key", tls::file_format::pem ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// mTLS Client Context
//
//------------------------------------------------------------------------------

// Client with client certificate for mTLS
tls::context make_mtls_client()
{
    tls::context ctx;
    
    // Client credentials for mTLS
    ctx.use_certificate_file( "client.crt", tls::file_format::pem ).value();
    ctx.use_private_key_file( "client.key", tls::file_format::pem ).value();
    
    // Trust specific CA for server verification
    ctx.load_verify_file( "server-ca.crt" ).value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Protocol Version Configuration
//
//------------------------------------------------------------------------------

// TLS 1.3 only
tls::context make_tls13_only()
{
    tls::context ctx;
    
    ctx.set_min_protocol_version( tls::version::tls_1_3 ).value();
    ctx.set_max_protocol_version( tls::version::tls_1_3 ).value();
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

// Allow TLS 1.2+ (default behavior made explicit)
tls::context make_tls12_plus()
{
    tls::context ctx;
    
    ctx.set_min_protocol_version( tls::version::tls_1_2 ).value();
    // No max = allow newest
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Cipher Suite Configuration
//
//------------------------------------------------------------------------------

// High-security cipher configuration
tls::context make_high_security()
{
    tls::context ctx;
    
    // Only ECDHE key exchange with AESGCM or ChaCha20
    ctx.set_ciphersuites( "ECDHE+AESGCM:ECDHE+CHACHA20" ).value();
    
    // TLS 1.3 only
    ctx.set_min_protocol_version( tls::version::tls_1_3 ).value();
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Revocation Checking
//
//------------------------------------------------------------------------------

// Client with CRL checking
tls::context make_client_with_crl( std::string_view crl_path )
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.add_crl_file( crl_path ).value();
    
    // Fail if certificate is revoked, allow if status unknown
    ctx.set_revocation_policy( tls::revocation_policy::soft_fail );
    
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

// Client requiring OCSP stapling
tls::context make_client_require_ocsp()
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Require server to provide OCSP staple
    ctx.set_require_ocsp_staple( true );
    
    return ctx;
}

// Server with OCSP stapling
tls::context make_server_with_ocsp( std::string_view ocsp_response )
{
    tls::context ctx;
    
    ctx.use_certificate_chain_file( "server.crt" ).value();
    ctx.use_private_key_file( "server.key", tls::file_format::pem ).value();
    
    // Provide pre-fetched OCSP response to clients
    ctx.set_ocsp_staple( ocsp_response ).value();
    
    return ctx;
}

// Strict revocation checking
tls::context make_hardened_client()
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Fail if revocation status cannot be determined
    ctx.set_revocation_policy( tls::revocation_policy::hard_fail );
    
    // Require OCSP staple from server
    ctx.set_require_ocsp_staple( true );
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Custom Verification
//
//------------------------------------------------------------------------------

// Client with custom verification callback
tls::context make_client_custom_verify()
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    ctx.set_verify_callback(
        []( bool preverified, auto& verify_ctx ) -> bool
        {
            if( !preverified )
            {
                // Standard verification failed - could log here
                return false;
            }
            
            // Additional custom checks could go here:
            // - Check specific certificate fields
            // - Verify against a pinned certificate
            // - Log certificate details
            
            return true;
        }).value();
    
    return ctx;
}

// Verify depth limit
tls::context make_client_limited_depth()
{
    tls::context ctx;
    
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Allow at most 2 intermediate certificates
    ctx.set_verify_depth( 2 ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Loading from Memory
//
//------------------------------------------------------------------------------

// Load all credentials from memory buffers
tls::context make_from_memory(
    std::string_view cert_pem,
    std::string_view key_pem,
    std::string_view ca_pem )
{
    tls::context ctx;
    
    // From vault/secret manager
    ctx.use_certificate_chain( cert_pem ).value();
    ctx.use_private_key( key_pem, tls::file_format::pem ).value();
    ctx.add_certificate_authority( ca_pem ).value();
    
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

// Load PKCS#12 from memory
tls::context make_from_pkcs12_memory(
    std::string_view pkcs12_data,
    std::string_view passphrase )
{
    tls::context ctx;
    
    ctx.use_pkcs12( pkcs12_data, passphrase ).value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// DER Format
//
//------------------------------------------------------------------------------

// Load DER-encoded certificate and key
tls::context make_from_der()
{
    tls::context ctx;
    
    ctx.use_certificate_file( "server.der", tls::file_format::der ).value();
    ctx.use_private_key_file( "server.key.der", tls::file_format::der ).value();
    
    return ctx;
}

//------------------------------------------------------------------------------
//
// Shared Context
//
//------------------------------------------------------------------------------

// Demonstrate shared ownership
void demonstrate_sharing()
{
    // Create a context
    tls::context original;
    original.set_default_verify_paths().value();
    original.set_verify_mode( tls::verify_mode::peer ).value();
    
    // Share via copy - both point to same underlying state
    tls::context copy1 = original;
    tls::context copy2 = original;
    
    // Changes to copy1 affect copy2 and original
    // (they all share the same impl)
    
    // Move transfers ownership
    tls::context moved = std::move( original );
    // original is now empty
}

//------------------------------------------------------------------------------
//
// Error Handling Patterns
//
//------------------------------------------------------------------------------

// Throw on error (simple code, let exceptions propagate)
void load_throwing()
{
    tls::context ctx;
    
    ctx.use_certificate_chain_file( "cert.pem" ).value();  // throws on error
    ctx.use_private_key_file( "key.pem", tls::file_format::pem ).value();
    ctx.set_default_verify_paths().value();
}

// Check errors explicitly
bool load_checked( tls::context& ctx, std::string& error_msg )
{
    if( auto r = ctx.use_certificate_chain_file( "cert.pem" ); !r )
    {
        error_msg = "Certificate: " + std::string( r.error().message() );
        return false;
    }
    
    if( auto r = ctx.use_private_key_file( "key.pem", tls::file_format::pem ); !r )
    {
        error_msg = "Key: " + std::string( r.error().message() );
        return false;
    }
    
    if( auto r = ctx.set_default_verify_paths(); !r )
    {
        error_msg = "CA store: " + std::string( r.error().message() );
        return false;
    }
    
    return true;
}

// Mixed approach - throw for programmer errors, check for runtime errors
void load_mixed()
{
    tls::context ctx;
    
    // File loading might fail at runtime
    if( auto r = ctx.use_certificate_chain_file( "cert.pem" ); !r )
    {
        // Handle missing file gracefully
        std::cerr << "Certificate not found: " << r.error().message() << "\n";
        return;
    }
    
    // Protocol settings won't fail if arguments are valid
    ctx.set_min_protocol_version( tls::version::tls_1_2 ).value();
    ctx.set_verify_mode( tls::verify_mode::peer ).value();
}

//------------------------------------------------------------------------------
//
// Main (not compiled, just for documentation)
//
//------------------------------------------------------------------------------

int main()
{
    // These examples demonstrate API ergonomics
    
    auto https = make_https_client();
    auto server = make_basic_server();
    auto mtls = make_mtls_server();
    
    return 0;
}
