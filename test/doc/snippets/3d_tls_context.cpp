//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/3.tutorials/3d.tls-context.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::assume[]
// The password-from-environment fragment uses std::getenv, which the
// Windows CRT deprecates under -Werror.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;
using namespace boost::corosio;
// end::assume[]

#include <boost/corosio/tls_stream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "test_suite.hpp"

namespace {

// Missing credential files make the load calls report an error code,
// which the fragments deliberately ignore; nothing throws.
void
construction()
{
    // tag::construction[]
    // Create a default context
    tls_context ctx;

    // Copy shares the same underlying state
    tls_context ctx2 = ctx;  // ctx and ctx2 share state

    // Move transfers ownership
    tls_context ctx3 = std::move( ctx );
    // ctx is now empty
    // end::construction[]
}

void
typical_setup()
{
    // tag::typical_setup[]
    tls_context ctx;

    // 1. Load credentials (for servers, or clients using client certs)
    ctx.use_certificate_chain_file( "server.crt" );
    ctx.use_private_key_file( "server.key", tls_file_format::pem );

    // 2. Configure trust anchors (for verifying peer certificates)
    ctx.set_default_verify_paths();  // Use system CA store

    // 3. Set verification mode
    ctx.set_verify_mode( tls_verify_mode::peer );

    // 4. Configure protocol options (optional)
    ctx.set_min_protocol_version( tls_version::tls_1_2 );
    // end::typical_setup[]
}

void
load_separate(tls_context& ctx)
{
    // tag::load_separate[]
    // Load certificate chain (leaf + intermediates)
    ctx.use_certificate_chain_file( "fullchain.pem" );

    // Load the matching private key
    ctx.use_private_key_file( "privkey.key", tls_file_format::pem );
    // end::load_separate[]
}

void
load_single(tls_context& ctx)
{
    // tag::load_single[]
    ctx.use_certificate_file( "server.crt", tls_file_format::pem );
    ctx.use_private_key_file( "server.key", tls_file_format::pem );
    // end::load_single[]
}

void
pkcs12_bundle(tls_context& ctx)
{
    // tag::pkcs12_file[]
    ctx.use_pkcs12_file( "credentials.pfx", "bundle-password" );
    // end::pkcs12_file[]
}

std::string
fetch_certificate_from_vault()
{
    return {};
}

std::string
fetch_key_from_vault()
{
    return {};
}

void
load_memory(tls_context& ctx)
{
    // tag::load_memory[]
    std::string cert_pem = fetch_certificate_from_vault();
    std::string key_pem = fetch_key_from_vault();

    ctx.use_certificate_chain( cert_pem );
    ctx.use_private_key( key_pem, tls_file_format::pem );
    // end::load_memory[]
}

void
der_files(tls_context& ctx)
{
    // tag::der_files[]
    ctx.use_certificate_file( "server.der", tls_file_format::der );
    ctx.use_private_key_file( "server.key.der", tls_file_format::der );
    // end::der_files[]
}

void
system_trust(tls_context& ctx)
{
    // tag::system_trust[]
    ctx.set_default_verify_paths();
    // end::system_trust[]
}

void
ca_bundle(tls_context& ctx)
{
    // tag::ca_bundle[]
    // Load CA bundle file (may contain multiple CAs)
    ctx.load_verify_file( "/path/to/ca-bundle.crt" );
    // end::ca_bundle[]
}

void
ca_directory(tls_context& ctx)
{
    // tag::ca_directory[]
    ctx.add_verify_path( "/etc/ssl/certs" );
    // end::ca_directory[]
}

std::string
load_ca_from_config()
{
    return {};
}

void
ca_individual(
    tls_context& ctx,
    std::string const& root_ca_pem,
    std::string const& intermediate_ca_pem)
{
    // tag::ca_individual[]
    // From memory
    std::string internal_ca = load_ca_from_config();
    ctx.add_certificate_authority( internal_ca );

    // Multiple CAs
    ctx.add_certificate_authority( root_ca_pem );
    ctx.add_certificate_authority( intermediate_ca_pem );
    // end::ca_individual[]
}

void
combine_trust(tls_context& ctx, std::string const& corporate_ca_pem)
{
    // tag::combine_trust[]
    // Start with system trust store
    ctx.set_default_verify_paths();

    // Add an internal CA for corporate servers
    ctx.add_certificate_authority( corporate_ca_pem );
    // end::combine_trust[]
}

void
version_bounds(tls_context& ctx)
{
    // tag::version_bounds[]
    // Require TLS 1.2 or newer (default)
    ctx.set_min_protocol_version( tls_version::tls_1_2 );

    // Require TLS 1.3 only
    ctx.set_min_protocol_version( tls_version::tls_1_3 );
    ctx.set_max_protocol_version( tls_version::tls_1_3 );
    // end::version_bounds[]
}

void
cipher_suites(tls_context& ctx)
{
    // tag::cipher_suites[]
    // TLS 1.2 and below
    ctx.set_ciphersuites( "ECDHE+AESGCM:ECDHE+CHACHA20" );

    // TLS 1.3 (distinct API and suite names)
    ctx.set_ciphersuites_tls13( "TLS_AES_256_GCM_SHA384" );
    // end::cipher_suites[]
}

void
alpn_offer(tls_context& ctx)
{
    // tag::alpn_offer[]
    // HTTP/2 with HTTP/1.1 fallback
    ctx.set_alpn( { "h2", "http/1.1" } );

    // gRPC
    ctx.set_alpn( { "h2" } );
    // end::alpn_offer[]
}

// Reading the negotiated protocol needs a stream whose handshake
// completed against a real peer; compiled but never executed.
[[maybe_unused]] void
alpn_read(corosio::tls_stream& stream)
{
    // tag::alpn_read[]
    std::string_view proto = stream.alpn_protocol(); // "h2", or empty if none
    // end::alpn_read[]
}

void
verify_modes(tls_context& ctx)
{
    // tag::verify_modes[]
    // Don't verify peer (not recommended for production)
    ctx.set_verify_mode( tls_verify_mode::none );

    // Verify peer if certificate is presented
    ctx.set_verify_mode( tls_verify_mode::peer );

    // Require and verify peer certificate (mTLS server-side)
    ctx.set_verify_mode( tls_verify_mode::require_peer );
    // end::verify_modes[]
}

// Setting a hostname needs a TLS stream, which needs a backend
// library; compiled but never executed.
[[maybe_unused]] void
hostname_setup(corosio::tls_stream& secure)
{
    // tag::set_hostname[]
    secure.set_hostname( "api.example.com" );
    // end::set_hostname[]
}

void
verify_depth(tls_context& ctx)
{
    // tag::verify_depth[]
    // Allow up to 3 intermediates (leaf -> 3 intermediates -> root)
    ctx.set_verify_depth( 3 );
    // end::verify_depth[]
}

std::vector<unsigned char> const expected_pin;

void
verify_callback(tls_context& ctx)
{
    // tag::verify_callback[]
    ctx.set_verify_callback(
        []( bool preverified, corosio::verify_context& verify_ctx ) -> bool
        {
            if( !preverified )
                return false;                    // chain did not verify

            auto der = verify_ctx.certificate(); // DER of the current cert
            return der.size() == expected_pin.size() &&
                std::equal( der.begin(), der.end(), expected_pin.begin() );
        });
    // end::verify_callback[]
}

void
revocation_policy(tls_context& ctx)
{
    // tag::revocation_policy[]
    // Don't check revocation (default)
    ctx.set_revocation_policy( tls_revocation_policy::disabled );

    // Accept unknown status, reject a listed (revoked) certificate
    ctx.set_revocation_policy( tls_revocation_policy::soft_fail );

    // Also reject when status can't be determined (strict)
    ctx.set_revocation_policy( tls_revocation_policy::hard_fail );
    // end::revocation_policy[]
}

std::string
fetch_crl_from_url(std::string_view)
{
    return {};
}

void
crl_load(tls_context& ctx, std::string_view crl_url)
{
    // tag::crl_load[]
    // From file
    ctx.add_crl_file( "/path/to/issuer.crl" );

    // From memory (e.g., fetched via HTTP)
    std::string crl_data = fetch_crl_from_url( crl_url );
    ctx.add_crl( crl_data );

    ctx.set_revocation_policy( tls_revocation_policy::hard_fail );
    // end::crl_load[]
}

void
bootstrap_hardened()
{
    // tag::bootstrap_hardened[]
    // Bootstrap context: for fetching revocation data
    tls_context bootstrap_ctx;
    bootstrap_ctx.set_default_verify_paths();
    bootstrap_ctx.set_verify_mode( tls_verify_mode::peer );
    bootstrap_ctx.set_revocation_policy( tls_revocation_policy::disabled );

    // Hardened context: for sensitive connections
    tls_context hardened_ctx;
    hardened_ctx.set_default_verify_paths();
    hardened_ctx.set_verify_mode( tls_verify_mode::peer );
    hardened_ctx.add_crl_file( "cached.crl" );
    hardened_ctx.set_revocation_policy( tls_revocation_policy::hard_fail );
    // end::bootstrap_hardened[]
}

void
password_callback(tls_context& ctx)
{
    // tag::password_callback[]
    // Set callback before loading encrypted key
    ctx.set_password_callback(
        []( std::size_t max_length, tls_password_purpose purpose )
        {
            // purpose: for_reading (decrypt) or for_writing (encrypt)
            return std::string( "my-secret-password" );
        });

    // Now load encrypted private key
    ctx.use_private_key_file( "encrypted.key", tls_file_format::pem );
    // end::password_callback[]
}

std::string
prompt_user_for_password()
{
    return {};
}

void
password_env(tls_context& ctx)
{
    // tag::password_env[]
    ctx.set_password_callback(
        []( std::size_t max_length, tls_password_purpose purpose )
        {
            // Read from environment
            if( auto* pw = std::getenv( "TLS_KEY_PASSWORD" ) )
                return std::string( pw );

            // Or prompt user
            return prompt_user_for_password();
        });
    // end::password_env[]
}

void
pkcs12_memory(tls_context& ctx, std::string_view pkcs12_data)
{
    // tag::pkcs12_memory[]
    ctx.use_pkcs12( pkcs12_data, "bundle-password" );
    // end::pkcs12_memory[]
}

// Throws std::system_error when the credential files are absent, as
// they are under the test runner; compiled but never executed.
[[maybe_unused]] void
error_handling(tls_context& ctx)
{
    // tag::error_handling[]
    // Throw on error
    if( auto ec = ctx.use_certificate_file( "cert.pem", tls_file_format::pem ) )
        throw std::system_error(ec);

    // Check error explicitly
    if( auto ec = ctx.load_verify_file( "ca.crt" ) )
    {
        std::cerr << "Failed to load CA: " << ec.message() << "\n";
        return;
    }
    // end::error_handling[]
}

struct tls_context_3d_test
{
    void
    run()
    {
        // Configuration calls record settings and report failures as
        // error codes, so every context-only fragment executes safely
        // without credential files or a TLS peer.
        tls_context ctx;
        construction();
        typical_setup();
        load_separate(ctx);
        load_single(ctx);
        pkcs12_bundle(ctx);
        load_memory(ctx);
        der_files(ctx);
        system_trust(ctx);
        ca_bundle(ctx);
        ca_directory(ctx);
        ca_individual(ctx, {}, {});
        combine_trust(ctx, {});
        version_bounds(ctx);
        cipher_suites(ctx);
        alpn_offer(ctx);
        verify_modes(ctx);
        verify_depth(ctx);
        verify_callback(ctx);
        revocation_policy(ctx);
        crl_load(ctx, {});
        bootstrap_hardened();
        password_callback(ctx);
        password_env(ctx);
        pkcs12_memory(ctx, {});
        BOOST_TEST(true);
    }
};

} // namespace

TEST_SUITE(tls_context_3d_test, "boost.corosio.doc.3d_tls_context");
