//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_CONTEXT_HPP
#define BOOST_COROSIO_TLS_CONTEXT_HPP

#include <boost/corosio/detail/config.hpp>

#include <cstddef>
#include <functional>
#include <span>
#include <system_error>
#include <memory>
#include <string_view>

namespace boost::corosio {

//
// Enumerations
//

/** TLS handshake role.

    Specifies whether to perform the TLS handshake as a client or server.

    @see stream::handshake
*/
enum class tls_role
{
    /// Perform handshake as the connecting client.
    client,

    /// Perform handshake as the accepting server.
    server
};

/** TLS protocol version.

    Specifies the minimum or maximum TLS protocol version to use
    for connections. Only modern, secure versions are supported.

    @see tls_context::set_min_protocol_version
    @see tls_context::set_max_protocol_version
*/
enum class tls_version
{
    /// TLS 1.2 (RFC 5246).
    tls_1_2,

    /// TLS 1.3 (RFC 8446).
    tls_1_3
};

/** Certificate and key file format.

    Specifies the encoding format for certificate and key data.

    @see tls_context::use_certificate
    @see tls_context::use_private_key
*/
enum class tls_file_format
{
    /// PEM format (Base64-encoded with header/footer lines).
    pem,

    /// DER format (raw ASN.1 binary encoding).
    der
};

/** Peer certificate verification mode.

    Controls how the TLS implementation verifies the peer's
    certificate during the handshake.

    @see tls_context::set_verify_mode
*/
enum class tls_verify_mode
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

    @see tls_context::set_revocation_policy
*/
enum class tls_revocation_policy
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

    @see tls_context::set_password_callback
*/
enum class tls_password_purpose
{
    /// Password needed to decrypt/read protected key material.
    for_reading,

    /// Password needed to encrypt/write protected key material.
    for_writing
};

class tls_context;

/** A non-owning view of certificate verification state.

    An instance is passed to the callback installed via
    tls_context::set_verify_callback during the TLS handshake. It
    exposes the backend's native verification handle so the callback
    can inspect the certificate and chain currently being verified.

    The value returned by native_handle() is, for the OpenSSL and
    WolfSSL backends, an `X509_STORE_CTX*`. For portable inspection that
    works across backends (for example certificate pinning), prefer
    certificate(), which returns the DER encoding of the certificate
    currently being verified.

    @par Lifetime

    The wrapped handle and the certificate() bytes are owned by the TLS
    backend and are valid only for the duration of a single callback
    invocation. Do not retain them beyond the call.

    @see tls_context::set_verify_callback
*/
class verify_context
{
    void* handle_;
    unsigned char const* der_;
    std::size_t der_len_;

public:
    /** Construct from a native handle and the current certificate.

        @param handle The backend verification handle (for OpenSSL and
            WolfSSL, an `X509_STORE_CTX*`).
        @param der Pointer to the DER encoding of the certificate under
            verification, or `nullptr` if unavailable.
        @param der_len Length of the DER encoding in bytes.
    */
    verify_context(
        void* handle, unsigned char const* der, std::size_t der_len) noexcept
        : handle_(handle), der_(der), der_len_(der_len)
    {
    }

    /** Return the native verification handle.

        Cast the result to the backend's verification context type
        (e.g. `X509_STORE_CTX*`) to inspect the certificate chain using
        backend-specific APIs.

        @return The native handle, or `nullptr` if none is available.
    */
    void* native_handle() const noexcept { return handle_; }

    /** Return the DER encoding of the certificate being verified.

        This is the portable way to inspect the peer certificate from a
        verification callback: it works identically on every backend,
        without depending on backend-specific build options. A DER
        certificate is an ASN.1 `SEQUENCE`, so the first byte is `0x30`.

        @return A view of the certificate's DER bytes, valid only for the
            duration of the callback. Empty if the certificate is not
            available.
    */
    std::span<unsigned char const> certificate() const noexcept
    {
        return {der_, der_len_};
    }
};

namespace detail {
struct tls_context_data;
tls_context_data const& get_tls_context_data(tls_context const&) noexcept;
} // namespace detail

/** A portable TLS context for certificate and settings storage.

    The `tls_context` class provides a backend-agnostic interface for
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
    corosio::tls_context ctx;
    ctx.set_default_verify_paths();
    ctx.set_verify_mode( corosio::tls_verify_mode::peer );

    // Use with a TLS stream
    corosio::openssl_stream secure( &sock, ctx );
    secure.set_hostname( "example.com" );
    co_await secure.handshake( corosio::tls_stream::client );
    @endcode

    @see tls_role
*/
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // shared_ptr needs dll-interface
#endif
class BOOST_COROSIO_DECL tls_context
{
    struct impl;
    std::shared_ptr<impl> impl_;

    friend detail::tls_context_data const&
    detail::get_tls_context_data(tls_context const&) noexcept;

public:
    /** Construct a default TLS context.

        Creates a context with default settings suitable for TLS 1.2
        and TLS 1.3 connections. No certificates or trust anchors are
        loaded; call the appropriate methods to configure credentials
        and verification.

        @par Example
        @code
        corosio::tls_context ctx;
        @endcode
    */
    tls_context();

    /** Copy constructor.

        Creates a new handle that shares ownership of the underlying
        TLS context state with `other`.

        @param other The context to copy from.
    */
    tls_context(tls_context const& other) = default;

    /** Copy assignment operator.

        Releases the current context's shared ownership and acquires
        shared ownership of `other`'s underlying state.

        @param other The context to copy from.

        @return Reference to this context.
    */
    tls_context& operator=(tls_context const& other) = default;

    /** Move constructor.

        Transfers ownership of the TLS context from another instance.
        After the move, `other` is in a valid but empty state.

        @param other The context to move from.
    */
    tls_context(tls_context&& other) noexcept = default;

    /** Move assignment operator.

        Releases the current context's shared ownership and transfers
        ownership from another instance. After the move, `other` is
        in a valid but empty state.

        @param other The context to move from.

        @return Reference to this context.
    */
    tls_context& operator=(tls_context&& other) noexcept = default;

    /** Destructor.

        Releases this handle's shared ownership of the underlying
        context. The context state is destroyed when the last handle
        is released.
    */
    ~tls_context() = default;

    //
    // Credential Loading
    //

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
    std::error_code
    use_certificate(std::string_view certificate, tls_file_format format);

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
        ctx.use_certificate_file( "server.crt", tls_file_format::pem );
        @endcode

        @see use_certificate
        @see use_private_key_file
    */
    std::error_code
    use_certificate_file(std::string_view filename, tls_file_format format);

    /** Load a certificate chain from a memory buffer.

        Loads the entity certificate followed by intermediate CA certificates.
        The chain should be ordered from leaf to root (excluding the root).
        This is the typical format for PEM certificate bundles.

        @param chain The certificate chain data in PEM format (concatenated
            certificates).

        @return Success, or an error if the chain could not be parsed.

        @see use_certificate_chain_file
    */
    std::error_code use_certificate_chain(std::string_view chain);

    /** Load a certificate chain from a file.

        Loads the entity certificate followed by intermediate CA certificates
        from a PEM file. The file should contain concatenated PEM certificates
        ordered from leaf to root (excluding the root).

        @param filename Path to the certificate chain file.

        @return Success, or an error if the file could not be read or parsed.

        @par Example
        @code
        // Load certificate chain (cert + intermediates)
        ctx.use_certificate_chain_file( "fullchain.pem" );
        @endcode

        @see use_certificate_chain
    */
    std::error_code use_certificate_chain_file(std::string_view filename);

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
    std::error_code
    use_private_key(std::string_view private_key, tls_file_format format);

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
        ctx.use_private_key_file( "server.key", tls_file_format::pem );
        @endcode

        @see use_private_key
        @see set_password_callback
    */
    std::error_code
    use_private_key_file(std::string_view filename, tls_file_format format);

    /** Load credentials from a PKCS#12 bundle in memory.

        PKCS#12 (also known as PFX) is a binary format that bundles a
        certificate, private key, and optionally intermediate certificates
        into a single password-protected file.

        @param data The PKCS#12 bundle data.

        @param passphrase The password protecting the bundle.

        @return Success. The bundle is recorded and decoded into the
            certificate, private key, and chain when the native context is
            first built; a malformed bundle or wrong passphrase surfaces as
            a handshake failure.

        @note Intermediate certificates inside the bundle are loaded and
            sent during the handshake on both backends.

        @see use_pkcs12_file
    */
    std::error_code
    use_pkcs12(std::string_view data, std::string_view passphrase);

    /** Load credentials from a PKCS#12 file.

        PKCS#12 (also known as PFX) is a binary format that bundles a
        certificate, private key, and optionally intermediate certificates
        into a single password-protected file. This is common on Windows
        and for certificates exported from browsers.

        @param filename Path to the PKCS#12 file.

        @param passphrase The password protecting the file.

        @return Success, or an error if the file could not be read. The
            bundle is decoded when the native context is first built; a
            malformed bundle or wrong passphrase surfaces as a handshake
            failure.

        @note Intermediate certificates inside the bundle are loaded and
            sent during the handshake on both backends.

        @par Example
        @code
        ctx.use_pkcs12_file( "credentials.pfx", "secret" );
        @endcode

        @see use_pkcs12
    */
    std::error_code
    use_pkcs12_file(std::string_view filename, std::string_view passphrase);

    //
    // Trust Anchors
    //

    /** Add a certificate authority for peer verification.

        Adds a single CA certificate to the trust store used for verifying
        peer certificates. Call this multiple times to add multiple CAs,
        or use `load_verify_file()` for a bundle.

        @param ca The CA certificate data in PEM format.

        @return Success, or an error if the certificate could not be parsed.

        @see load_verify_file
        @see set_default_verify_paths
    */
    std::error_code add_certificate_authority(std::string_view ca);

    /** Load CA certificates from a file.

        Loads one or more CA certificates from a PEM file. The file may
        contain multiple concatenated PEM certificates.

        @param filename Path to a PEM file containing CA certificates.

        @return Success, or an error if the file could not be read or parsed.

        @par Example
        @code
        // Load a custom CA bundle
        ctx.load_verify_file( "/etc/ssl/certs/ca-certificates.crt" );
        @endcode

        @see add_certificate_authority
        @see add_verify_path
    */
    std::error_code load_verify_file(std::string_view filename);

    /** Add a directory of CA certificates for verification.

        Adds a directory of CA certificates to the trust store. The
        directory is applied when the native context is first built from
        this context.

        The expected directory layout depends on the backend. OpenSSL
        performs on-demand lookups and requires each certificate file to
        be named by its subject-name hash (as generated by
        `openssl rehash` or `c_rehash`); WolfSSL loads every certificate
        file in the directory.

        @param path Path to the directory of CA certificates.

        @return Success. The path is recorded and applied when the native
            context is built; a directory that cannot be read at that time
            is skipped rather than reported here.

        @par Example
        @code
        ctx.add_verify_path( "/etc/ssl/certs" );
        @endcode

        @see load_verify_file
        @see set_default_verify_paths
    */
    std::error_code add_verify_path(std::string_view path);

    /** Use the system default CA certificate store.

        Configures the context to use the operating system's default
        trust store for peer certificate verification. This is the
        recommended approach for HTTPS clients connecting to public
        servers.

        The system store is loaded when the native context is first built
        from this context. For a verified-safe client, combine this with
        `set_verify_mode( tls_verify_mode::peer )` and, when connecting by
        name, `tls_stream::set_hostname()`.

        @return Success. The request is recorded and applied when the
            native context is built; if the system store cannot be loaded
            at that time it is skipped rather than reported here, so a
            context that must reject unverified peers should also use
            `set_verify_mode( tls_verify_mode::peer )`.

        @note The OpenSSL backend honors the `SSL_CERT_FILE` and
            `SSL_CERT_DIR` environment variables. The WolfSSL backend
            requires a build with `WOLFSSL_SYS_CA_CERTS`; without it the
            system store is unavailable and this call has no effect.

        @par Example
        @code
        // Trust the same CAs as the system
        ctx.set_default_verify_paths();
        ctx.set_verify_mode( tls_verify_mode::peer );
        @endcode

        @see load_verify_file
        @see add_verify_path
        @see set_verify_mode
    */
    std::error_code set_default_verify_paths();

    //
    // Protocol Configuration
    //

    /** Set the minimum TLS protocol version.

        Connections will reject protocol versions older than this.
        The default allows TLS 1.2 and newer.

        @param v The minimum protocol version to accept.

        @return Success, or an error if the version is not supported
            by the backend.

        @par Example
        @code
        // Require TLS 1.3 minimum
        ctx.set_min_protocol_version( tls_version::tls_1_3 );
        @endcode

        @see set_max_protocol_version
    */
    std::error_code set_min_protocol_version(tls_version v);

    /** Set the maximum TLS protocol version.

        Connections will not negotiate protocol versions newer than this.
        The default allows the newest supported version.

        @param v The maximum protocol version to accept.

        @return Success, or an error if the version is not supported
            by the backend.

        @note On WolfSSL the ceiling is applied by selecting a
            version-specific method (no native set-max API exists); an
            invalid window where the minimum exceeds the maximum yields a
            context that fails the handshake.

        @see set_min_protocol_version
    */
    std::error_code set_max_protocol_version(tls_version v);

    /** Set the allowed cipher suites.

        Configures which cipher suites may be used for connections.
        The format is backend-specific but typically follows OpenSSL
        cipher list syntax.

        @param ciphers The cipher suite specification string.

        @return Success, or an error if the cipher string is invalid.

        @par Example
        @code
        // TLS 1.2 cipher suites (OpenSSL format)
        ctx.set_ciphersuites( "ECDHE+AESGCM:ECDHE+CHACHA20" );
        @endcode

        @note This configures cipher suites for TLS 1.2 and below. For
            TLS 1.3, use @ref set_ciphersuites_tls13.
    */
    std::error_code set_ciphersuites(std::string_view ciphers);

    /** Set the allowed TLS 1.3 cipher suites.

        TLS 1.3 uses a distinct, fixed set of cipher suites configured
        separately from earlier versions. The format is a colon-separated
        list of TLS 1.3 suite names.

        @param ciphers The TLS 1.3 cipher suite list.

        @return Success, or an error if the cipher string is invalid.

        @par Example
        @code
        ctx.set_ciphersuites_tls13(
            "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256" );
        @endcode

        @note On the WolfSSL backend, TLS 1.2 and TLS 1.3 suites share a
            single cipher list; this call and @ref set_ciphersuites are
            merged into one list.

        @see set_ciphersuites
    */
    std::error_code set_ciphersuites_tls13(std::string_view ciphers);

    /** Set the ALPN protocol list.

        Configures Application-Layer Protocol Negotiation (ALPN) for
        the connection. ALPN is used to negotiate which application
        protocol to use over the TLS connection (e.g., "h2" for HTTP/2,
        "http/1.1" for HTTP/1.1).

        The protocols are tried in preference order (first = highest).

        @param protocols Ordered list of protocol identifiers.

        @return Success, or an error if ALPN configuration fails.

        @note Read the negotiated protocol after the handshake via
            @ref tls_stream::alpn_protocol. On WolfSSL, ALPN requires a
            build with `HAVE_ALPN`; without it, offering protocols fails
            the handshake with `std::errc::function_not_supported` rather
            than negotiate nothing silently.

        @par Example
        @code
        // Prefer HTTP/2, fall back to HTTP/1.1
        ctx.set_alpn( { "h2", "http/1.1" } );
        @endcode
    */
    std::error_code set_alpn(std::initializer_list<std::string_view> protocols);

    //
    // Certificate Verification
    //

    /** Set the peer certificate verification mode.

        Controls whether and how peer certificates are verified during
        the TLS handshake.

        @param mode The verification mode to use.

        @return Success, or an error if the mode could not be set.

        @par Example
        @code
        // Verify peer certificate (typical for clients)
        ctx.set_verify_mode( tls_verify_mode::peer );

        // Require client certificate (server-side mTLS)
        ctx.set_verify_mode( tls_verify_mode::require_peer );
        @endcode

        @see tls_verify_mode
    */
    std::error_code set_verify_mode(tls_verify_mode mode);

    /** Set the maximum certificate chain verification depth.

        Limits how many intermediate certificates can appear between
        the peer certificate and a trusted root. The default is
        typically 100, which is sufficient for most certificate chains.

        @param depth Maximum number of intermediate certificates allowed.

        @return Success, or an error if the depth is invalid.
    */
    std::error_code set_verify_depth(int depth);

    /** Set a custom certificate verification callback.

        Installs a callback that is invoked during certificate chain
        verification. The callback can perform additional validation
        beyond the standard checks and can override verification
        results.

        The callback receives the built-in verification result so far and
        a verify_context describing the certificate being verified. Return
        `true` to accept the certificate, `false` to reject. Inspect the
        certificate portably via `verify_context::certificate()` (its DER
        encoding) — for example to pin a specific certificate.

        @par Backend Support

        The exact set of certificates the callback sees differs by backend:

        - OpenSSL: the callback runs once per certificate in the chain,
          including certificates that passed the built-in checks. It can
          therefore both relax verification (return `true` for a
          certificate the library rejected) and tighten it (return `false`
          for a certificate the library accepted, e.g. pinning).
        - WolfSSL built with `WOLFSSL_ALWAYS_VERIFY_CB` (implied by
          `--enable-opensslextra`): same as OpenSSL.
        - WolfSSL without that option: the library invokes the callback
          only on verification *failure*, so it cannot be honored on a
          successful handshake. To avoid silently ignoring a
          verification-tightening callback (which would fail open), a
          context that carries a callback instead **fails the handshake**
          with `std::errc::function_not_supported` on such a build. Rebuild
          WolfSSL with `WOLFSSL_ALWAYS_VERIFY_CB`, or omit the callback.

        @tparam Callback A callable with signature
            `bool( bool preverified, verify_context& ctx )`.

        @param callback The verification callback.

        @return Success. The callback is recorded here and applied during the
            handshake. On a WolfSSL build that cannot honor it, the handshake
            fails with `std::errc::function_not_supported` (see Backend
            Support).

        @par Example
        @code
        ctx.set_verify_mode( tls_verify_mode::peer );
        ctx.set_verify_callback(
            []( bool preverified, verify_context& ctx ) -> bool
            {
                if( ! preverified )
                    return false;
                // Pin: accept only a certificate whose DER matches.
                auto der = ctx.certificate();
                return der.size() == expected_pin.size() &&
                    std::equal( der.begin(), der.end(), expected_pin.begin() );
            });
        @endcode

        @see verify_context
        @see set_verify_mode
    */
    template<typename Callback>
    std::error_code set_verify_callback(Callback callback);

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

        @see tls_stream::set_hostname
    */
    template<typename Callback>
    void set_servername_callback(Callback callback);

private:
    void set_servername_callback_impl(
        std::function<bool(std::string_view)> callback);

    void set_password_callback_impl(
        std::function<std::string(std::size_t, tls_password_purpose)> callback);

    void set_verify_callback_impl(
        std::function<bool(bool, verify_context&)> callback);

public:
    //
    // Revocation Checking
    //

    /** Add a Certificate Revocation List from memory.

        Adds a CRL to the verification store for checking whether
        certificates have been revoked. CRLs are typically fetched
        from the URLs in a certificate's CRL Distribution Points
        extension.

        @param crl The CRL data in DER or PEM format.

        @return Success, or an error if the CRL could not be parsed.

        @note CRLs are consulted only when a revocation policy is set via
            @ref set_revocation_policy. On WolfSSL, CRL checking requires a
            build with `HAVE_CRL`; without it, supplying a CRL or a
            revocation policy fails the handshake with
            `std::errc::function_not_supported`.

        @see add_crl_file
        @see set_revocation_policy
    */
    std::error_code add_crl(std::string_view crl);

    /** Add a Certificate Revocation List from a file.

        Adds a CRL to the verification store for checking whether
        certificates have been revoked.

        @param filename Path to a CRL file (DER or PEM format).

        @return Success, or an error if the file could not be read
            or the CRL is invalid.

        @note CRLs are consulted only when a revocation policy is set via
            @ref set_revocation_policy (WolfSSL requires a `HAVE_CRL`
            build).

        @par Example
        @code
        ctx.add_crl_file( "issuer.crl" );
        @endcode

        @see add_crl
        @see set_revocation_policy
    */
    std::error_code add_crl_file(std::string_view filename);

    /** Set the certificate revocation checking policy.

        Controls how certificate revocation status is checked during
        verification via CRLs.

        @param policy The revocation checking policy.

        @par Example
        @code
        // Require successful revocation check
        ctx.set_revocation_policy( tls_revocation_policy::hard_fail );

        // Check but allow unknown status
        ctx.set_revocation_policy( tls_revocation_policy::soft_fail );
        @endcode

        @note Revocation is checked via CRLs supplied with @ref add_crl /
            @ref add_crl_file. `soft_fail` accepts a certificate whose
            status cannot be determined (missing/expired CRL) but rejects
            one that is actually revoked; `hard_fail` also rejects unknown
            status. OCSP-based revocation is not available (see the TLS
            guide). On WolfSSL a non-disabled policy requires a `HAVE_CRL`
            build, else the handshake fails with
            `std::errc::function_not_supported`.

        @see tls_revocation_policy
        @see add_crl
    */
    void set_revocation_policy(tls_revocation_policy policy);

    //
    // Password Handling
    //

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
            []( std::size_t max_len, tls_password_purpose purpose )
            {
                // In practice, prompt user or read from secure storage
                return std::string( "my-key-password" );
            });

        // Now load encrypted key
        ctx.use_private_key_file( "encrypted.key", tls_file_format::pem );
        @endcode

        @see tls_password_purpose
    */
    template<typename Callback>
    void set_password_callback(Callback callback);
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

template<typename Callback>
void
tls_context::set_servername_callback(Callback callback)
{
    set_servername_callback_impl(std::move(callback));
}

template<typename Callback>
void
tls_context::set_password_callback(Callback callback)
{
    set_password_callback_impl(std::move(callback));
}

template<typename Callback>
std::error_code
tls_context::set_verify_callback(Callback callback)
{
    set_verify_callback_impl(std::move(callback));
    return {};
}

} // namespace boost::corosio

#endif
