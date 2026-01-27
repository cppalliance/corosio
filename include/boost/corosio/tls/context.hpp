//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_CONTEXT_HPP
#define BOOST_COROSIO_TLS_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/system/result.hpp>

#include <functional>
#include <memory>
#include <string_view>

namespace boost::corosio::tls {

//------------------------------------------------------------------------------
//
// Enumerations
//
//------------------------------------------------------------------------------

/** TLS handshake role.

    Specifies whether to perform the TLS handshake as a client or server.

    @see stream::handshake
*/
enum class role
{
    /// Perform handshake as the connecting client.
    client,

    /// Perform handshake as the accepting server.
    server
};

/** TLS protocol version.

    Specifies the minimum or maximum TLS protocol version to use
    for connections. Only modern, secure versions are supported.

    @see context::set_min_protocol_version
    @see context::set_max_protocol_version
*/
enum class version
{
    /// TLS 1.2 (RFC 5246).
    tls_1_2,

    /// TLS 1.3 (RFC 8446).
    tls_1_3
};

/** Certificate and key file format.

    Specifies the encoding format for certificate and key data.

    @see context::use_certificate
    @see context::use_private_key
*/
enum class file_format
{
    /// PEM format (Base64-encoded with header/footer lines).
    pem,

    /// DER format (raw ASN.1 binary encoding).
    der
};

/** Peer certificate verification mode.

    Controls how the TLS implementation verifies the peer's
    certificate during the handshake.

    @see context::set_verify_mode
*/
enum class verify_mode
{
    /// Do not request or verify the peer certificate.
    none,

    /// Request and verify the peer certificate if presented.
    peer,

    /// Require and verify the peer certificate (fail if not presented).
    require_peer
};

/** Certificate revocation checking policy.

    Controls how certificate revocation status is checked during
    verification.

    @see context::set_revocation_policy
*/
enum class revocation_policy
{
    /// Do not check revocation status.
    disabled,

    /// Check revocation but allow connection if status is unknown.
    soft_fail,

    /// Require successful revocation check (fail if status is unknown).
    hard_fail
};

/** Purpose for password callback invocation.

    Indicates whether the password is needed for reading (decrypting)
    or writing (encrypting) key material.

    @see context::set_password_callback
*/
enum class password_purpose
{
    /// Password needed to decrypt/read protected key material.
    for_reading,

    /// Password needed to encrypt/write protected key material.
    for_writing
};

class context;

namespace detail {
struct context_data;
context_data const&
get_context_data( context const& ) noexcept;
} // namespace detail

/** A portable TLS context for certificate and settings storage.

    The `context` class provides a backend-agnostic interface for
    configuring TLS connections. It stores credentials (certificates and
    private keys), trust anchors, protocol settings, and verification
    options that are used when establishing TLS connections.

    This class is a shared handle to an opaque implementation. Copies
    share the same underlying state. This allows contexts to be passed
    by value and shared across multiple TLS streams.

    This class abstracts the configuration phase of TLS across multiple
    backend implementations (OpenSSL, WolfSSL, mbedTLS, Schannel, etc.),
    allowing portable code that works regardless of which TLS library
    is linked.

    @par Modification After Stream Creation

    Modifying a context after a TLS stream has been created from it
    results in undefined behavior. The context's configuration is
    captured when the first stream is constructed, and subsequent
    modifications are not reflected in existing or new streams
    sharing the context.

    If different configurations are needed, create separate context
    objects.

    @par Thread Safety

    Distinct objects: Safe.

    Shared objects: Unsafe. A context must not be modified while
    any thread is creating streams from it.

    @par Example
    @code
    // Create a client context with system trust anchors
    corosio::tls::context ctx;
    ctx.set_default_verify_paths().value();
    ctx.set_verify_mode( corosio::tls::verify_mode::peer ).value();
    ctx.set_hostname( "example.com" );

    // Use with a TLS stream
    corosio::tls::stream secure( sock, ctx );
    co_await secure.handshake( corosio::tls::role::client );
    @endcode

    @see role
*/
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4251)  // shared_ptr needs dll-interface
#endif
class BOOST_COROSIO_DECL context
{
    struct impl;
    std::shared_ptr<impl> impl_;

    friend
    detail::context_data const&
    detail::get_context_data( context const& ) noexcept;

public:
    /** Construct a default TLS context.

        Creates a context with default settings suitable for TLS 1.2
        and TLS 1.3 connections. No certificates or trust anchors are
        loaded; call the appropriate methods to configure credentials
        and verification.

        @par Example
        @code
        corosio::tls::context ctx;
        @endcode
    */
    context();

    /** Copy constructor.

        Creates a new handle that shares ownership of the underlying
        TLS context state with `other`.

        @param other The context to copy from.
    */
    context( context const& other ) = default;

    /** Copy assignment operator.

        Releases the current context's shared ownership and acquires
        shared ownership of `other`'s underlying state.

        @param other The context to copy from.

        @return Reference to this context.
    */
    context& operator=( context const& other ) = default;

    /** Move constructor.

        Transfers ownership of the TLS context from another instance.
        After the move, `other` is in a valid but empty state.

        @param other The context to move from.
    */
    context( context&& other ) noexcept = default;

    /** Move assignment operator.

        Releases the current context's shared ownership and transfers
        ownership from another instance. After the move, `other` is
        in a valid but empty state.

        @param other The context to move from.

        @return Reference to this context.
    */
    context& operator=( context&& other ) noexcept = default;

    /** Destructor.

        Releases this handle's shared ownership of the underlying
        context. The context state is destroyed when the last handle
        is released.
    */
    ~context() = default;

    //--------------------------------------------------------------------------
    //
    // Credential Loading
    //
    //--------------------------------------------------------------------------

    /** Load the entity certificate from a memory buffer.

        Sets the certificate that identifies this endpoint to the peer.
        For servers, this is the server certificate. For clients using
        mutual TLS, this is the client certificate.

        The certificate must match the private key loaded via
        `use_private_key()` or `use_private_key_file()`.

        @param certificate The certificate data.

        @param format The encoding format of the certificate data.

        @return Success, or an error if the certificate could not be parsed
            or is invalid.

        @see use_certificate_file
        @see use_private_key
    */
    system::result<void>
    use_certificate(
        std::string_view certificate,
        file_format format );

    /** Load the entity certificate from a file.

        Sets the certificate that identifies this endpoint to the peer.
        For servers, this is the server certificate. For clients using
        mutual TLS, this is the client certificate.

        @param filename Path to the certificate file.

        @param format The encoding format of the file.

        @return Success, or an error if the file could not be read or the
            certificate is invalid.

        @par Example
        @code
        ctx.use_certificate_file( "server.crt", tls::file_format::pem ).value();
        @endcode

        @see use_certificate
        @see use_private_key_file
    */
    system::result<void>
    use_certificate_file(
        std::string_view filename,
        file_format format );

    /** Load a certificate chain from a memory buffer.

        Loads the entity certificate followed by intermediate CA certificates.
        The chain should be ordered from leaf to root (excluding the root).
        This is the typical format for PEM certificate bundles.

        @param chain The certificate chain data in PEM format (concatenated
            certificates).

        @return Success, or an error if the chain could not be parsed.

        @see use_certificate_chain_file
    */
    system::result<void>
    use_certificate_chain( std::string_view chain );

    /** Load a certificate chain from a file.

        Loads the entity certificate followed by intermediate CA certificates
        from a PEM file. The file should contain concatenated PEM certificates
        ordered from leaf to root (excluding the root).

        @param filename Path to the certificate chain file.

        @return Success, or an error if the file could not be read or parsed.

        @par Example
        @code
        // Load certificate chain (cert + intermediates)
        ctx.use_certificate_chain_file( "fullchain.pem" ).value();
        @endcode

        @see use_certificate_chain
    */
    system::result<void>
    use_certificate_chain_file( std::string_view filename );

    /** Load the private key from a memory buffer.

        Sets the private key corresponding to the entity certificate.
        The key must match the certificate loaded via `use_certificate()`
        or `use_certificate_chain()`.

        If the key is encrypted, set a password callback via
        `set_password_callback()` before calling this function.

        @param private_key The private key data.

        @param format The encoding format of the key data.

        @return Success, or an error if the key could not be parsed,
            is encrypted without a password callback, or doesn't match
            the certificate.

        @see use_private_key_file
        @see set_password_callback
    */
    system::result<void>
    use_private_key(
        std::string_view private_key,
        file_format format );

    /** Load the private key from a file.

        Sets the private key corresponding to the entity certificate.
        The key must match the certificate loaded via `use_certificate_file()`
        or `use_certificate_chain_file()`.

        If the key file is encrypted, set a password callback via
        `set_password_callback()` before calling this function.

        @param filename Path to the private key file.

        @param format The encoding format of the file.

        @return Success, or an error if the file could not be read,
            the key is invalid, or it doesn't match the certificate.

        @par Example
        @code
        ctx.use_private_key_file( "server.key", tls::file_format::pem ).value();
        @endcode

        @see use_private_key
        @see set_password_callback
    */
    system::result<void>
    use_private_key_file(
        std::string_view filename,
        file_format format );

    /** Load credentials from a PKCS#12 bundle in memory.

        PKCS#12 (also known as PFX) is a binary format that bundles a
        certificate, private key, and optionally intermediate certificates
        into a single password-protected file.

        @param data The PKCS#12 bundle data.

        @param passphrase The password protecting the bundle.

        @return Success, or an error if the bundle could not be parsed
            or the passphrase is incorrect.

        @see use_pkcs12_file
    */
    system::result<void>
    use_pkcs12(
        std::string_view data,
        std::string_view passphrase );

    /** Load credentials from a PKCS#12 file.

        PKCS#12 (also known as PFX) is a binary format that bundles a
        certificate, private key, and optionally intermediate certificates
        into a single password-protected file. This is common on Windows
        and for certificates exported from browsers.

        @param filename Path to the PKCS#12 file.

        @param passphrase The password protecting the file.

        @return Success, or an error if the file could not be read,
            parsed, or the passphrase is incorrect.

        @par Example
        @code
        ctx.use_pkcs12_file( "credentials.pfx", "secret" ).value();
        @endcode

        @see use_pkcs12
    */
    system::result<void>
    use_pkcs12_file(
        std::string_view filename,
        std::string_view passphrase );

    //--------------------------------------------------------------------------
    //
    // Trust Anchors
    //
    //--------------------------------------------------------------------------

    /** Add a certificate authority for peer verification.

        Adds a single CA certificate to the trust store used for verifying
        peer certificates. Call this multiple times to add multiple CAs,
        or use `load_verify_file()` for a bundle.

        @param ca The CA certificate data in PEM format.

        @return Success, or an error if the certificate could not be parsed.

        @see load_verify_file
        @see set_default_verify_paths
    */
    system::result<void>
    add_certificate_authority( std::string_view ca );

    /** Load CA certificates from a file.

        Loads one or more CA certificates from a PEM file. The file may
        contain multiple concatenated PEM certificates.

        @param filename Path to a PEM file containing CA certificates.

        @return Success, or an error if the file could not be read or parsed.

        @par Example
        @code
        // Load a custom CA bundle
        ctx.load_verify_file( "/etc/ssl/certs/ca-certificates.crt" ).value();
        @endcode

        @see add_certificate_authority
        @see add_verify_path
    */
    system::result<void>
    load_verify_file( std::string_view filename );

    /** Add a directory of CA certificates for verification.

        Adds a directory containing CA certificate files. Each file must
        contain a single certificate in PEM format, named using the
        subject name hash (as generated by `openssl rehash` or
        `c_rehash`).

        @param path Path to the directory containing hashed CA certificates.

        @return Success, or an error if the directory is invalid.

        @par Example
        @code
        ctx.add_verify_path( "/etc/ssl/certs" ).value();
        @endcode

        @see load_verify_file
        @see set_default_verify_paths
    */
    system::result<void>
    add_verify_path( std::string_view path );

    /** Use the system default CA certificate store.

        Configures the context to use the operating system's default
        trust store for peer certificate verification. This is the
        recommended approach for HTTPS clients connecting to public
        servers.

        On different platforms this uses:
        - Linux: `/etc/ssl/certs` or distribution-specific paths
        - macOS: System Keychain
        - Windows: Windows Certificate Store

        @return Success, or an error if the system store could not be loaded.

        @par Example
        @code
        // Trust the same CAs as the system
        ctx.set_default_verify_paths().value();
        @endcode

        @see load_verify_file
        @see add_verify_path
    */
    system::result<void>
    set_default_verify_paths();

    //--------------------------------------------------------------------------
    //
    // Protocol Configuration
    //
    //--------------------------------------------------------------------------

    /** Set the minimum TLS protocol version.

        Connections will reject protocol versions older than this.
        The default allows TLS 1.2 and newer.

        @param v The minimum protocol version to accept.

        @return Success, or an error if the version is not supported
            by the backend.

        @par Example
        @code
        // Require TLS 1.3 minimum
        ctx.set_min_protocol_version( tls::version::tls_1_3 ).value();
        @endcode

        @see set_max_protocol_version
    */
    system::result<void>
    set_min_protocol_version( version v );

    /** Set the maximum TLS protocol version.

        Connections will not negotiate protocol versions newer than this.
        The default allows the newest supported version.

        @param v The maximum protocol version to accept.

        @return Success, or an error if the version is not supported
            by the backend.

        @see set_min_protocol_version
    */
    system::result<void>
    set_max_protocol_version( version v );

    /** Set the allowed cipher suites.

        Configures which cipher suites may be used for connections.
        The format is backend-specific but typically follows OpenSSL
        cipher list syntax.

        @param ciphers The cipher suite specification string.

        @return Success, or an error if the cipher string is invalid.

        @par Example
        @code
        // TLS 1.2 cipher suites (OpenSSL format)
        ctx.set_ciphersuites( "ECDHE+AESGCM:ECDHE+CHACHA20" ).value();
        @endcode

        @note For TLS 1.3, use `set_ciphersuites_tls13()` on backends
            that distinguish between TLS 1.2 and 1.3 cipher configuration.
    */
    system::result<void>
    set_ciphersuites( std::string_view ciphers );

    /** Set the ALPN protocol list.

        Configures Application-Layer Protocol Negotiation (ALPN) for
        the connection. ALPN is used to negotiate which application
        protocol to use over the TLS connection (e.g., "h2" for HTTP/2,
        "http/1.1" for HTTP/1.1).

        The protocols are tried in preference order (first = highest).

        @param protocols Ordered list of protocol identifiers.

        @return Success, or an error if ALPN configuration fails.

        @par Example
        @code
        // Prefer HTTP/2, fall back to HTTP/1.1
        ctx.set_alpn( { "h2", "http/1.1" } ).value();
        @endcode
    */
    system::result<void>
    set_alpn( std::initializer_list<std::string_view> protocols );

    //--------------------------------------------------------------------------
    //
    // Certificate Verification
    //
    //--------------------------------------------------------------------------

    /** Set the peer certificate verification mode.

        Controls whether and how peer certificates are verified during
        the TLS handshake.

        @param mode The verification mode to use.

        @return Success, or an error if the mode could not be set.

        @par Example
        @code
        // Verify peer certificate (typical for clients)
        ctx.set_verify_mode( tls::verify_mode::peer ).value();

        // Require client certificate (server-side mTLS)
        ctx.set_verify_mode( tls::verify_mode::require_peer ).value();
        @endcode

        @see verify_mode
    */
    system::result<void>
    set_verify_mode( verify_mode mode );

    /** Set the maximum certificate chain verification depth.

        Limits how many intermediate certificates can appear between
        the peer certificate and a trusted root. The default is
        typically 100, which is sufficient for most certificate chains.

        @param depth Maximum number of intermediate certificates allowed.

        @return Success, or an error if the depth is invalid.
    */
    system::result<void>
    set_verify_depth( int depth );

    /** Set a custom certificate verification callback.

        Installs a callback that is invoked during certificate chain
        verification. The callback can perform additional validation
        beyond the standard checks and can override verification
        results.

        The callback receives the verification result so far and
        information about the certificate being verified. Return
        `true` to accept the certificate, `false` to reject.

        @tparam Callback A callable with signature
            `bool( bool preverified, verify_context& ctx )`.

        @param callback The verification callback.

        @return Success, or an error if the callback could not be set.

        @note The `verify_context` type provides access to the
            certificate and chain information. Its exact interface
            depends on the TLS backend.
    */
    template<typename Callback>
    system::result<void>
    set_verify_callback( Callback callback );

    /** Set the expected server hostname for verification.

        For client connections, sets the hostname that the server
        certificate must match. This enables:

        1. SNI (Server Name Indication) — tells the server which
           certificate to present (for virtual hosting)
        2. Hostname verification — validates the certificate's
           Subject Alternative Name or Common Name matches

        @param hostname The expected server hostname.

        @par Example
        @code
        ctx.set_hostname( "api.example.com" );
        @endcode

        @note This is typically required for HTTPS clients to ensure
            they're connecting to the intended server.
    */
    void
    set_hostname( std::string_view hostname );

    /** Set a callback for Server Name Indication (SNI).

        For server connections, this callback is invoked during the TLS
        handshake when a client sends an SNI extension. The callback
        receives the requested hostname and can accept or reject the
        connection.

        @tparam Callback A callable with signature
            `bool( std::string_view hostname )`.

        @param callback The SNI callback. Return `true` to accept the
            connection or `false` to reject it with an alert.

        @par Example
        @code
        // Accept connections for specific domains only
        ctx.set_servername_callback(
            []( std::string_view hostname ) -> bool
            {
                return hostname == "api.example.com" ||
                       hostname == "www.example.com";
            });
        @endcode

        @note For virtual hosting with different certificates per hostname,
            create separate contexts and select the appropriate one before
            creating the TLS stream.

        @see set_hostname
    */
    template<typename Callback>
    void
    set_servername_callback( Callback callback );

private:
    void
    set_servername_callback_impl(
        std::function<bool( std::string_view )> callback );

public:

    //--------------------------------------------------------------------------
    //
    // Revocation Checking
    //
    //--------------------------------------------------------------------------

    /** Add a Certificate Revocation List from memory.

        Adds a CRL to the verification store for checking whether
        certificates have been revoked. CRLs are typically fetched
        from the URLs in a certificate's CRL Distribution Points
        extension.

        @param crl The CRL data in DER or PEM format.

        @return Success, or an error if the CRL could not be parsed.

        @see add_crl_file
        @see set_revocation_policy
    */
    system::result<void>
    add_crl( std::string_view crl );

    /** Add a Certificate Revocation List from a file.

        Adds a CRL to the verification store for checking whether
        certificates have been revoked.

        @param filename Path to a CRL file (DER or PEM format).

        @return Success, or an error if the file could not be read
            or the CRL is invalid.

        @par Example
        @code
        ctx.add_crl_file( "issuer.crl" ).value();
        @endcode

        @see add_crl
        @see set_revocation_policy
    */
    system::result<void>
    add_crl_file( std::string_view filename );

    /** Set the OCSP staple response for server-side stapling.

        For servers, provides a pre-fetched OCSP response to send
        to clients during the handshake. This proves the server's
        certificate hasn't been revoked without requiring the client
        to contact the OCSP responder.

        The OCSP response must be periodically refreshed (typically
        every few hours to days) before it expires.

        @param response The DER-encoded OCSP response.

        @return Success, or an error if the response is invalid.

        @note This is a server-side operation. Clients use
            `set_require_ocsp_staple()` to require stapled responses.
    */
    system::result<void>
    set_ocsp_staple( std::string_view response );

    /** Require OCSP stapling from the server.

        For clients, requires the server to provide a stapled OCSP
        response proving its certificate hasn't been revoked. If
        the server doesn't provide a stapled response, the handshake
        fails.

        @param require Whether to require OCSP stapling.

        @note Not all servers support OCSP stapling. Enable this only
            when connecting to servers known to support it.
    */
    void
    set_require_ocsp_staple( bool require );

    /** Set the certificate revocation checking policy.

        Controls how certificate revocation status is checked during
        verification. This affects both CRL and OCSP checking.

        @param policy The revocation checking policy.

        @par Example
        @code
        // Require successful revocation check
        ctx.set_revocation_policy( tls::revocation_policy::hard_fail );

        // Check but allow unknown status
        ctx.set_revocation_policy( tls::revocation_policy::soft_fail );
        @endcode

        @see revocation_policy
        @see add_crl
    */
    void
    set_revocation_policy( revocation_policy policy );

    //--------------------------------------------------------------------------
    //
    // Password Handling
    //
    //--------------------------------------------------------------------------

    /** Set the password callback for encrypted keys.

        Installs a callback that provides passwords for encrypted
        private keys and PKCS#12 files. The callback is invoked when
        loading encrypted key material.

        @tparam Callback A callable with signature
            `std::string( std::size_t max_length, password_purpose purpose )`.

        @param callback The password callback. It receives the maximum
            password length and the purpose (reading or writing), and
            returns the password string.

        @par Example
        @code
        ctx.set_password_callback(
            []( std::size_t max_len, tls::password_purpose purpose )
            {
                // In practice, prompt user or read from secure storage
                return std::string( "my-key-password" );
            });

        // Now load encrypted key
        ctx.use_private_key_file( "encrypted.key", tls::file_format::pem ).value();
        @endcode

        @see password_purpose
    */
    template<typename Callback>
    void
    set_password_callback( Callback callback );
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

template<typename Callback>
void
context::
set_servername_callback( Callback callback )
{
    set_servername_callback_impl( std::move( callback ) );
}

} // namespace boost::corosio::tls

#endif
