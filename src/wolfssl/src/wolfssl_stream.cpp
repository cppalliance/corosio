//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/write.hpp>

// Internal context implementation
#include "src/tls/detail/context_impl.hpp"

// Include WolfSSL options first to get proper feature detection
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/pkcs12.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

/*
    wolfssl_stream Architecture
    ===========================

    TLS layer wrapping an underlying stream (via any_stream). Supports one
    concurrent read_some and one concurrent write_some (like Asio's ssl::stream).

    Data Flow
    ---------
    App -> wolfSSL_write -> send_callback -> out_buf_ -> s_->write_some -> Network
    App <- wolfSSL_read  <- recv_callback <- in_buf_  <- s_->read_some  <- Network

    WANT_READ / WANT_WRITE Pattern
    ------------------------------
    WolfSSL's I/O callbacks are synchronous but our underlying stream is async.
    When WolfSSL needs I/O:

      1. Callback checks internal buffer (in_buf_ or out_buf_)
      2. If data available: return it immediately
      3. If not: return WOLFSSL_CBIO_ERR_WANT_READ or WANT_WRITE
      4. wolfSSL_read/write returns WOLFSSL_ERROR_WANT_*
      5. Our coroutine does async I/O: co_await s_->read_some() or write_some()
      6. Loop back to step 1

    Renegotiation causes cross-direction I/O: SSL_read may need to write
    handshake data, SSL_write may need to read. Each operation handles
    whatever I/O direction WolfSSL requests.

    WolfSSL Context Initialization (IMPORTANT)
    ------------------------------------------
    Unlike OpenSSL which provides a combined TLS_method() for both client and
    server roles, standard WolfSSL builds only expose separate methods:
      - wolfTLS_client_method()  -- for client connections
      - wolfTLS_server_method()  -- for server connections

    The combined wolfSSLv23_method() requires WolfSSL to be built with
    --enable-opensslextra or --enable-opensslall, which many distributions omit.

    To handle this portably:
      1. wolfssl_native_context caches TWO WOLFSSL_CTX pointers (client + server)
      2. The WOLFSSL object is NOT created at stream construction time
      3. Instead, init_ssl_for_role(type) is called at handshake time when we
         know whether this is a client or server connection
      4. This deferred initialization selects the appropriate cached context

    This design allows a single tls_context to be shared across both client
    and server streams without requiring OpenSSL compatibility mode in WolfSSL.
*/

namespace boost::corosio {

namespace {

// Default buffer size for TLS I/O
constexpr std::size_t default_buffer_size = 16384;

inline bool
is_zero_return_error(int err) noexcept
{
    return err == WOLFSSL_ERROR_ZERO_RETURN;
}

inline bool
has_peer_shutdown(WOLFSSL* ssl) noexcept
{
    return wolfSSL_get_shutdown(ssl) != 0;
}

class wolfssl_category_impl final : public std::error_category
{
    char const*
    name() const noexcept override
    {
        return "corosio.wolfssl";
    }

    std::string
    message(int value) const override
    {
        char buf[WOLFSSL_MAX_ERROR_SZ];
        wolfSSL_ERR_error_string_n(
            static_cast<unsigned long>(value), buf, sizeof(buf));
        return buf;
    }
};

} // namespace

std::error_category const&
wolfssl_category() noexcept
{
    static wolfssl_category_impl instance;
    return instance;
}

bool
wolfssl_supports_verify_callback() noexcept
{
#if defined(WOLFSSL_ALWAYS_VERIFY_CB)
    return true;
#else
    return false;
#endif
}

bool
wolfssl_supports_alpn() noexcept
{
#if defined(HAVE_ALPN)
    return true;
#else
    return false;
#endif
}

bool
wolfssl_supports_crl() noexcept
{
#if defined(HAVE_CRL)
    return true;
#else
    return false;
#endif
}

//
// Native context caching
//

namespace detail {

// SNI callback invoked by WolfSSL during handshake (server-side)
// Returns SNICbReturn enum: 0 = OK, fatal_return (2) = abort
static int
wolfssl_sni_callback(WOLFSSL* ssl, int* /* alert */, void* arg)
{
    void* sni_data = nullptr;
    unsigned short sni_len =
        wolfSSL_SNI_GetRequest(ssl, WOLFSSL_SNI_HOST_NAME, &sni_data);
    if (!sni_data || sni_len == 0)
        return 0; // No SNI sent, continue

    std::string_view servername(static_cast<char const*>(sni_data), sni_len);

    auto* cd = static_cast<tls_context_data const*>(arg);
    if (cd && cd->servername_callback)
    {
        if (!cd->servername_callback(servername))
            return fatal_return; // Callback rejected hostname
    }

    return 0; // Accept
}

#if defined(HAVE_CRL)
// CRL error callback for the soft-fail revocation policy.
//
// WolfSSL invokes this ONLY when it cannot determine a certificate's
// revocation status because no CRL entry was found for it (ret ==
// CRL_MISSING, or an expired/undecodable CRL). Returning non-zero overrides
// that error to success. A certificate that is actually listed in a loaded
// CRL is rejected before this path (CRL_CERT_REVOKED), so it still fails
// closed. This mirrors OpenSSL's soft_fail: tolerate "status unknown",
// still reject "revoked".
static int
wolfssl_crl_soft_fail_cb(
    int /*ret*/, WOLFSSL_CRL* /*crl*/, WOLFSSL_CERT_MANAGER* /*cm*/,
    void* /*ctx*/)
{
    return 1; // override missing/unknown-status CRL error -> accept
}
#endif

#if defined(WOLFSSL_ALWAYS_VERIFY_CB)
// WOLFSSL_CTX ex_data slot holding the portable tls_context_data pointer,
// used to reach the user's verify callback from the trampoline. Allocated
// dynamically (see apply_common_settings) rather than hardcoded, because
// slot 0 is the OpenSSL-compat app-data slot.
static int verify_cd_ex_index = -1;

// Trampoline installed via wolfSSL_CTX_set_verify.
//
// This is only wired up when the linked WolfSSL was built with
// WOLFSSL_ALWAYS_VERIFY_CB (implied by --enable-opensslextra), which makes
// WolfSSL invoke the callback for every certificate including on success.
// Without it, WolfSSL calls the callback only on failure, so a callback
// that tightens verification would silently fail open; on such builds the
// callback is refused entirely (see init_ssl_for_role).
//
// The context data is recovered via the OpenSSL-compatible ex_data chain
// (store -> WOLFSSL -> WOLFSSL_CTX -> ex_data), which those builds provide.
// The native store->userCtx field is NOT used: it is left unset by WolfSSL
// on OPENSSL_EXTRA builds, so relying on it silently drops the callback.
static int
wolfssl_verify_callback(int preverified, WOLFSSL_X509_STORE_CTX* store)
{
    WOLFSSL* ssl = static_cast<WOLFSSL*>(wolfSSL_X509_STORE_CTX_get_ex_data(
        store, wolfSSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl)
        return preverified;

    WOLFSSL_CTX* wctx = wolfSSL_get_SSL_CTX(ssl);
    auto* cd          = wctx ? static_cast<tls_context_data const*>(
                             wolfSSL_CTX_get_ex_data(wctx, verify_cd_ex_index))
                             : nullptr;
    if (!cd || !cd->verify_callback)
        return preverified;

    // Expose the current certificate's DER so the callback can inspect it.
    // On OPENSSL_EXTRA builds (the only builds this trampoline runs on) the
    // current certificate is available via the compat API; get_der returns a
    // pointer into the certificate's own storage (no allocation, valid for
    // the callback's duration).
    unsigned char const* der = nullptr;
    std::size_t der_len       = 0;
    if (WOLFSSL_X509* cert = wolfSSL_X509_STORE_CTX_get_current_cert(store))
    {
        int sz             = 0;
        unsigned char const* d = wolfSSL_X509_get_der(cert, &sz);
        if (d && sz > 0)
        {
            der     = d;
            der_len = static_cast<std::size_t>(sz);
        }
    }

    verify_context vc(store, der, der_len);
    return cd->verify_callback(preverified != 0, vc) ? 1 : 0;
}
#endif // WOLFSSL_ALWAYS_VERIFY_CB

// Select the WolfSSL method for a [min,max] version window. WolfSSL has
// no native set_max_proto_version (that API needs OPENSSL_EXTRA), so the
// ceiling is expressed by choosing a version-specific method; the floor
// is additionally enforced via wolfSSL_CTX_SetMinVersion.
inline WOLFSSL_METHOD*
wolfssl_method_for(bool server, tls_version min_v, tls_version max_v)
{
    if (min_v == tls_version::tls_1_3)
        return server ? wolfTLSv1_3_server_method()
                      : wolfTLSv1_3_client_method();
    if (max_v == tls_version::tls_1_2)
        return server ? wolfTLSv1_2_server_method()
                      : wolfTLSv1_2_client_method();
    return server ? wolfTLS_server_method() : wolfTLS_client_method();
}

inline int
wolfssl_min_version_const(tls_version v) noexcept
{
    return v == tls_version::tls_1_3 ? WOLFSSL_TLSV1_3 : WOLFSSL_TLSV1_2;
}

/** Cached WolfSSL contexts owning WOLFSSL_CTX for client and server.

    Created on first stream construction for a given tls_context,
    then reused for subsequent streams sharing that context.
    Maintains separate contexts for client and server roles since
    WolfSSL requires different method functions for each.
*/
class wolfssl_native_context : public native_context_base
{
public:
    WOLFSSL_CTX* client_ctx_;
    WOLFSSL_CTX* server_ctx_;
    // Set when a requested configuration could not be applied: an inverted
    // protocol window (min > max), a cipher list the library rejected, a
    // version floor that would not set, or a CRL that parsed as neither PEM
    // nor DER. init_ssl_for_role then fails closed rather than negotiate an
    // unexpected version, ignore the requested ciphers, or weaken revocation
    // (fail-open under soft_fail).
    bool setup_failed_ = false;
    // ALPN protocol list in WolfSSL's comma-separated format, built once
    // from the immutable protocol list. Installed per-session (client offer
    // / server candidates); caching it avoids rebuilding per connection.
    std::string alpn_list_;

    void
    apply_common_settings(WOLFSSL_CTX* ctx, tls_context_data const& cd)
    {
        if (!ctx)
            return;

        // Apply verify mode from config
        int verify_mode_flag = WOLFSSL_VERIFY_NONE;
        if (cd.verification_mode == tls_verify_mode::peer)
            verify_mode_flag = WOLFSSL_VERIFY_PEER;
        else if (cd.verification_mode == tls_verify_mode::require_peer)
            verify_mode_flag =
                WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;

        // On capable builds install the verify trampoline and stash the
        // context data in the WOLFSSL_CTX ex_data, where the trampoline
        // recovers it. On builds that cannot honor the callback the
        // handshake is refused in init_ssl_for_role instead.
#if defined(WOLFSSL_ALWAYS_VERIFY_CB)
        if (cd.verify_callback)
        {
            if (verify_cd_ex_index < 0)
            {
#if defined(HAVE_EX_DATA_CRYPTO)
                // Allocate a dedicated ex_data slot when the API is available.
                verify_cd_ex_index = wolfSSL_CTX_get_ex_new_index(
                    0, nullptr, nullptr, nullptr, nullptr);
#endif
                // wolfSSL_CTX_get_ex_new_index requires HAVE_EX_DATA_CRYPTO,
                // which OPENSSL_EXTRA does not imply. Where it is unavailable
                // (or returned slot 0, the OpenSSL-compat app-data slot),
                // fall back to a fixed non-app-data slot. corosio stores no
                // other ex_data on this WOLFSSL_CTX.
                if (verify_cd_ex_index <= 0)
                    verify_cd_ex_index = 1;
            }
            wolfSSL_CTX_set_ex_data(
                ctx, verify_cd_ex_index, const_cast<tls_context_data*>(&cd));
            wolfSSL_CTX_set_verify(
                ctx, verify_mode_flag, wolfssl_verify_callback);
        }
        else
            wolfSSL_CTX_set_verify(ctx, verify_mode_flag, nullptr);
#else
        wolfSSL_CTX_set_verify(ctx, verify_mode_flag, nullptr);
#endif

        // PKCS#12 bundle: decode cert + key with the native wolfcrypt API
        // (the wolfSSL_d2i_PKCS12_bio wrapper needs OPENSSL_EXTRA). CA/chain
        // entries inside the bundle are loaded and sent during the handshake
        // (see below), matching the OpenSSL backend.
        if (!cd.pkcs12_data.empty())
        {
            // A bundle that fails to decode or parse (wrong passphrase,
            // malformed) must not leave the context silently credential-less:
            // a client using PKCS#12 for mTLS would then fail open against a
            // verify_mode::peer server. Fail closed like every other setup
            // error.
            WC_PKCS12* p12 = wc_PKCS12_new();
            if (!p12)
                setup_failed_ = true;
            else
            {
                byte* pkey         = nullptr;
                word32 pkeySz      = 0;
                byte* cert         = nullptr;
                word32 certSz      = 0;
                WC_DerCertList* ca = nullptr;
                if (wc_d2i_PKCS12(
                        reinterpret_cast<byte const*>(cd.pkcs12_data.data()),
                        static_cast<word32>(cd.pkcs12_data.size()), p12) != 0 ||
                    wc_PKCS12_parse(
                        p12, cd.pkcs12_password.c_str(), &pkey, &pkeySz, &cert,
                        &certSz, &ca) != 0)
                {
                    setup_failed_ = true;
                }
                else
                {
                    if (cert && ca)
                    {
                        // Concatenate leaf + chain into one DER blob and load
                        // it as a chain so the intermediate(s) are sent during
                        // the handshake (parity with the OpenSSL backend).
                        std::vector<unsigned char> chain(cert, cert + certSz);
                        for (WC_DerCertList* n = ca; n; n = n->next)
                            chain.insert(
                                chain.end(), n->buffer, n->buffer + n->bufferSz);
                        if (wolfSSL_CTX_use_certificate_chain_buffer_format(
                                ctx, chain.data(),
                                static_cast<long>(chain.size()),
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_failed_ = true;
                    }
                    else if (cert)
                    {
                        if (wolfSSL_CTX_use_certificate_buffer(
                                ctx, cert, static_cast<long>(certSz),
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_failed_ = true;
                    }
                    if (pkey &&
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx, pkey, static_cast<long>(pkeySz),
                            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                        setup_failed_ = true;
                    if (ca)
                        wc_FreeCertList(ca, nullptr);
                    XFREE(pkey, nullptr, DYNAMIC_TYPE_PKCS);
                    XFREE(cert, nullptr, DYNAMIC_TYPE_PKCS);
                }
                wc_PKCS12_free(p12);
            }
        }

        // Apply certificate chain if provided (entity cert + intermediates).
        // These discrete PEM/DER fields are an alternative credential source
        // to a PKCS#12 bundle; when a bundle was supplied it already loaded
        // the credential above, so skip them (parity with the OpenSSL path).
        if (cd.pkcs12_data.empty() && !cd.certificate_chain.empty())
        {
            wolfSSL_CTX_use_certificate_chain_buffer(
                ctx,
                reinterpret_cast<unsigned char const*>(
                    cd.certificate_chain.data()),
                static_cast<long>(cd.certificate_chain.size()));
        }
        else if (cd.pkcs12_data.empty() && !cd.entity_certificate.empty())
        {
            // Only use single certificate if no chain provided
            int format = (cd.entity_cert_format == tls_file_format::pem)
                ? WOLFSSL_FILETYPE_PEM
                : WOLFSSL_FILETYPE_ASN1;
            wolfSSL_CTX_use_certificate_buffer(
                ctx,
                reinterpret_cast<unsigned char const*>(
                    cd.entity_certificate.data()),
                static_cast<long>(cd.entity_certificate.size()), format);
        }

        // Apply private key if provided (skipped when a PKCS#12 bundle
        // already supplied the credential, as above).
        if (cd.pkcs12_data.empty() && !cd.private_key.empty())
        {
            if (cd.password_callback)
            {
                // Native wolfSSL APIs work without OPENSSL_EXTRA
                std::string password = cd.password_callback(
                    256, tls_password_purpose::for_reading);

                if (cd.private_key_format == tls_file_format::pem)
                {
                    std::vector<unsigned char> der_buf(cd.private_key.size());
                    int der_len = wc_KeyPemToDer(
                        reinterpret_cast<unsigned char const*>(
                            cd.private_key.data()),
                        static_cast<int>(cd.private_key.size()), der_buf.data(),
                        static_cast<int>(der_buf.size()), password.c_str());

                    if (der_len > 0)
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx, der_buf.data(), der_len,
                            WOLFSSL_FILETYPE_ASN1);
                }
                else
                {
                    // Encrypted PKCS#8 DER - decrypt in place on a copy
                    std::vector<unsigned char> der_buf(
                        cd.private_key.begin(), cd.private_key.end());
                    int dec_len = wc_DecryptPKCS8Key(
                        der_buf.data(), static_cast<word32>(der_buf.size()),
                        password.c_str(), static_cast<int>(password.size()));

                    if (dec_len > 0)
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx, der_buf.data(), dec_len,
                            WOLFSSL_FILETYPE_ASN1);
                    else
                        // Not encrypted or decryption failed - try loading directly
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx,
                            reinterpret_cast<unsigned char const*>(
                                cd.private_key.data()),
                            static_cast<long>(cd.private_key.size()),
                            WOLFSSL_FILETYPE_ASN1);
                }
            }
            else
            {
                int format = (cd.private_key_format == tls_file_format::pem)
                    ? WOLFSSL_FILETYPE_PEM
                    : WOLFSSL_FILETYPE_ASN1;
                wolfSSL_CTX_use_PrivateKey_buffer(
                    ctx,
                    reinterpret_cast<unsigned char const*>(
                        cd.private_key.data()),
                    static_cast<long>(cd.private_key.size()), format);
            }
        }

        // Apply CA certificates for verification
        for (auto const& ca : cd.ca_certificates)
        {
            wolfSSL_CTX_load_verify_buffer(
                ctx, reinterpret_cast<unsigned char const*>(ca.data()),
                static_cast<long>(ca.size()), WOLFSSL_FILETYPE_PEM);
        }

        // Trust anchors from the system store and explicit directories.
        // A failing source is left unloaded rather than aborting context
        // creation.
#ifdef WOLFSSL_SYS_CA_CERTS
        if (cd.use_default_verify_paths)
            wolfSSL_CTX_load_system_CA_certs(ctx);
#endif
        for (auto const& path : cd.verify_paths)
            wolfSSL_CTX_load_verify_locations(ctx, nullptr, path.c_str());

        // Enforce the version floor. The method chosen in the ctor sets the
        // ceiling; SetMinVersion pins the floor (the range method would
        // otherwise permit older versions the build still supports). An
        // inverted window (min > max) is silently reduced to a min-only
        // context by wolfssl_method_for, so catch it explicitly and fail
        // closed rather than negotiate an unexpected version.
        if (cd.min_version > cd.max_version)
            setup_failed_ = true;
        if (wolfSSL_CTX_SetMinVersion(
                ctx, wolfssl_min_version_const(cd.min_version)) !=
            WOLFSSL_SUCCESS)
            setup_failed_ = true;

        // Apply verify depth
        wolfSSL_CTX_set_verify_depth(ctx, cd.verify_depth);

        // Cipher configuration. WolfSSL accepts TLS 1.2 and TLS 1.3 suite
        // names in a single colon-separated list, so merge both fields.
        if (!cd.ciphersuites.empty() || !cd.ciphersuites_tls13.empty())
        {
            std::string list = cd.ciphersuites;
            if (!cd.ciphersuites_tls13.empty())
            {
                if (!list.empty())
                    list.push_back(':');
                list.append(cd.ciphersuites_tls13);
            }
            // A cipher list the library rejects must not silently fall back
            // to the default suites; fail closed instead.
            if (wolfSSL_CTX_set_cipher_list(ctx, list.c_str()) !=
                WOLFSSL_SUCCESS)
                setup_failed_ = true;
        }

#if defined(HAVE_CRL)
        // Certificate revocation via CRLs (fuller builds only; when HAVE_CRL
        // is absent, init_ssl_for_role fails closed instead).
        if (cd.revocation != tls_revocation_policy::disabled)
        {
            if (wolfSSL_CTX_EnableCRL(ctx, WOLFSSL_CRL_CHECK) !=
                WOLFSSL_SUCCESS)
                setup_failed_ = true;
            for (auto const& crl : cd.crls)
            {
                auto const* buf =
                    reinterpret_cast<unsigned char const*>(crl.data());
                auto const sz = static_cast<long>(crl.size());
                // Accept PEM or DER (the documented contract): try PEM, then
                // DER. A supplied CRL that parses as neither must not be
                // silently dropped, so record it for a fail-closed handshake.
                if (wolfSSL_CTX_LoadCRLBuffer(
                        ctx, buf, sz, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS &&
                    wolfSSL_CTX_LoadCRLBuffer(
                        ctx, buf, sz, WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                    setup_failed_ = true;
            }
            // soft_fail tolerates an undeterminable revocation status (no CRL
            // loaded for a cert) the way OpenSSL does. Without this, WolfSSL's
            // WOLFSSL_CRL_CHECK hard-fails with CRL_MISSING; the callback
            // downgrades that to success while still rejecting a cert that a
            // loaded CRL actually revokes.
            if (cd.revocation == tls_revocation_policy::soft_fail)
                wolfSSL_CTX_SetCRL_ErrorCb(
                    ctx, &detail::wolfssl_crl_soft_fail_cb, nullptr);
        }
#endif
    }

    tls_context_data const* cd_; // For SNI callback access

    explicit wolfssl_native_context(tls_context_data const& cd)
        : client_ctx_(nullptr)
        , server_ctx_(nullptr)
        , cd_(&cd)
    {
        // Create separate contexts for client and server, choosing the
        // method that honors the configured protocol version window.
        client_ctx_ = wolfSSL_CTX_new(
            wolfssl_method_for(false, cd.min_version, cd.max_version));
        server_ctx_ = wolfSSL_CTX_new(
            wolfssl_method_for(true, cd.min_version, cd.max_version));

        apply_common_settings(client_ctx_, cd);
        apply_common_settings(server_ctx_, cd);

        // Encode the ALPN protocol list once (comma-separated); each session
        // installs it via wolfSSL_UseALPN in init_ssl_for_role.
        for (auto const& p : cd.alpn_protocols)
        {
            if (!alpn_list_.empty())
                alpn_list_.push_back(',');
            alpn_list_.append(p);
        }

        // Set SNI callback on server context if provided
        if (server_ctx_ && cd.servername_callback)
        {
            wolfSSL_CTX_set_servername_callback(
                server_ctx_, wolfssl_sni_callback);
            wolfSSL_CTX_set_servername_arg(
                server_ctx_, const_cast<tls_context_data*>(&cd));
        }
    }

    ~wolfssl_native_context() override
    {
        if (client_ctx_)
            wolfSSL_CTX_free(client_ctx_);
        if (server_ctx_)
            wolfSSL_CTX_free(server_ctx_);
    }
};

/** Get or create cached wolfssl_native_context for this context.

    @param cd The context implementation.

    @return Pointer to the cached native context wrapper.
*/
inline wolfssl_native_context*
get_wolfssl_native_context(tls_context_data const& cd)
{
    static char key;
    auto* p = cd.find(&key, [&] { return new wolfssl_native_context(cd); });
    return static_cast<wolfssl_native_context*>(p);
}

} // namespace detail

struct wolfssl_stream::impl
{
    capy::any_stream* s_;
    tls_context ctx_;
    WOLFSSL* ssl_ = nullptr;
    bool used_    = false;

    // ALPN protocol negotiated during the handshake (empty if none).
    std::string alpn_selected_;

    // Buffers for read operations
    std::vector<char> read_in_buf_;
    std::size_t read_in_pos_ = 0;
    std::size_t read_in_len_ = 0;
    std::vector<char> read_out_buf_;
    std::size_t read_out_len_ = 0;

    // Buffers for write operations
    std::vector<char> write_in_buf_;
    std::size_t write_in_pos_ = 0;
    std::size_t write_in_len_ = 0;
    std::vector<char> write_out_buf_;
    std::size_t write_out_len_ = 0;

    // Thread-local pointer to current operation's buffers
    // Set before calling wolfSSL_read/write so callbacks know which buffers to use
    struct op_buffers
    {
        std::vector<char>* in_buf;
        std::size_t* in_pos;
        std::size_t* in_len;
        std::vector<char>* out_buf;
        std::size_t* out_len;
        bool want_read;
        bool want_write;
    };
    op_buffers* current_op_ = nullptr;

    // Renegotiation can cause both TLS read/write to access the socket
    capy::async_mutex io_cm_;

    impl(capy::any_stream& s, tls_context ctx) : s_(&s), ctx_(std::move(ctx))
    {
        read_in_buf_.resize(default_buffer_size);
        read_out_buf_.resize(default_buffer_size);
        write_in_buf_.resize(default_buffer_size);
        write_out_buf_.resize(default_buffer_size);
    }

    ~impl()
    {
        if (ssl_)
            wolfSSL_free(ssl_);
        // WOLFSSL_CTX* is owned by cached native context, not freed here
    }

    // Releases WOLFSSL object and resets buffer positions.
    // I/O buffer vectors keep their allocations.
    void reset()
    {
        if (ssl_)
        {
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
        }
        read_in_pos_   = 0;
        read_in_len_   = 0;
        read_out_len_  = 0;
        write_in_pos_  = 0;
        write_in_len_  = 0;
        write_out_len_ = 0;
        current_op_    = nullptr;
        alpn_selected_.clear();
        used_          = false;
    }

    // Record the ALPN protocol selected during the handshake, if any.
    void capture_alpn()
    {
#if defined(HAVE_ALPN)
        char* name       = nullptr;
        unsigned short sz = 0;
        if (wolfSSL_ALPN_GetProtocol(ssl_, &name, &sz) == WOLFSSL_SUCCESS &&
            name && sz)
            alpn_selected_.assign(name, sz);
#endif
    }

    // WolfSSL I/O Callbacks

    /** Callback invoked by WolfSSL when it needs to receive data.

        Returns data from the current operation's input buffer if available,
        otherwise returns WOLFSSL_CBIO_ERR_WANT_READ.
    */
    static int recv_callback(WOLFSSL*, char* buf, int sz, void* ctx)
    {
        auto* self = static_cast<impl*>(ctx);
        auto* op   = self->current_op_;

        // Check if we have data in the input buffer
        std::size_t available = *op->in_len - *op->in_pos;
        if (available == 0)
        {
            // No data available, signal need to read
            op->want_read = true;
            return WOLFSSL_CBIO_ERR_WANT_READ;
        }

        // Copy available data to WolfSSL's buffer
        std::size_t to_copy =
            (std::min)(available, static_cast<std::size_t>(sz));
        std::memcpy(buf, op->in_buf->data() + *op->in_pos, to_copy);
        *op->in_pos += to_copy;

        // If we've consumed all data, reset buffer position
        if (*op->in_pos == *op->in_len)
        {
            *op->in_pos = 0;
            *op->in_len = 0;
        }

        return static_cast<int>(to_copy);
    }

    /** Callback invoked by WolfSSL when it needs to send data.

        Copies data to the current operation's output buffer.
        Returns WOLFSSL_CBIO_ERR_WANT_WRITE if the buffer is full.
    */
    static int send_callback(WOLFSSL*, char* buf, int sz, void* ctx)
    {
        auto* self = static_cast<impl*>(ctx);
        auto* op   = self->current_op_;

        // Check if we have room in the output buffer
        std::size_t available = op->out_buf->size() - *op->out_len;
        if (available == 0)
        {
            // Buffer full, signal need to write
            op->want_write = true;
            return WOLFSSL_CBIO_ERR_WANT_WRITE;
        }

        // Copy data to output buffer
        std::size_t to_copy =
            (std::min)(available, static_cast<std::size_t>(sz));
        std::memcpy(op->out_buf->data() + *op->out_len, buf, to_copy);
        *op->out_len += to_copy;

        // If we couldn't copy everything, signal partial write
        if (to_copy < static_cast<std::size_t>(sz))
            op->want_write = true;

        return static_cast<int>(to_copy);
    }

    // Inner coroutines for TLS read/write operations

    capy::io_task<std::size_t>
    do_read_some(capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
    {
        std::error_code ec;
        std::size_t total_read = 0;

        // Set up operation buffers for callbacks
        op_buffers op{&read_in_buf_,  &read_in_pos_,  &read_in_len_,
                      &read_out_buf_, &read_out_len_, false,
                      false};
        current_op_ = &op;

        for (auto& buf : buffers)
        {
            char* dest    = static_cast<char*>(buf.data());
            int remaining = static_cast<int>(buf.size());

            while (remaining > 0)
            {
                op.want_read  = false;
                op.want_write = false;

                int ret = wolfSSL_read(ssl_, dest, remaining);

                if (ret > 0)
                {
                    // Successfully read some data
                    dest += ret;
                    remaining -= ret;
                    total_read += static_cast<std::size_t>(ret);

                    // For read_some semantics, return after first successful read
                    if (total_read > 0)
                    {
                        current_op_ = nullptr;
                        co_return {std::error_code{}, total_read};
                    }
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if (err == WOLFSSL_ERROR_WANT_READ)
                    {
                        if (read_in_pos_ == read_in_len_)
                        {
                            read_in_pos_ = 0;
                            read_in_len_ = 0;
                        }
                        capy::mutable_buffer rbuf(
                            read_in_buf_.data() + read_in_len_,
                            read_in_buf_.size() - read_in_len_);

                        auto [lec] = co_await io_cm_.lock();
                        if (lec)
                        {
                            current_op_ = nullptr;
                            co_return {lec, total_read};
                        }
                        capy::async_mutex::lock_guard io_guard(&io_cm_);
                        auto [rec, rn] = co_await s_->read_some(rbuf);
                        if (rec)
                        {
                            if (rec == make_error_code(capy::error::eof))
                            {
                                // Check if we got a proper TLS shutdown
                                if (has_peer_shutdown(ssl_))
                                    ec = make_error_code(capy::error::eof);
                                else
                                    ec = make_error_code(
                                        capy::error::stream_truncated);
                            }
                            else
                            {
                                ec = rec;
                            }
                            current_op_ = nullptr;
                            co_return {ec, total_read};
                        }
                        read_in_len_ += rn;
                    }
                    else if (err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Renegotiation
                        if (read_out_len_ > 0)
                        {
                            auto [lec] = co_await io_cm_.lock();
                            if (lec)
                            {
                                current_op_ = nullptr;
                                co_return {lec, total_read};
                            }
                            capy::async_mutex::lock_guard io_guard(&io_cm_);
                            auto [wec, wn] = co_await capy::write(
                                *s_,
                                capy::const_buffer(
                                    read_out_buf_.data(), read_out_len_));
                            read_out_len_ = 0;
                            if (wec)
                            {
                                current_op_ = nullptr;
                                co_return {wec, total_read};
                            }
                        }
                    }
                    else if (is_zero_return_error(err))
                    {
                        // Clean TLS shutdown - treat as EOF
                        current_op_ = nullptr;
                        co_return {
                            make_error_code(capy::error::eof), total_read};
                    }
                    else
                    {
                        // Other error
                        current_op_ = nullptr;
                        co_return {
                            std::error_code(err, wolfssl_category()),
                            total_read};
                    }
                }
            }
        }

        current_op_ = nullptr;
        co_return {std::error_code{}, total_read};
    }

    capy::io_task<std::size_t>
    do_write_some(capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
    {
        std::error_code ec;
        std::size_t total_written = 0;

        // Set up operation buffers for callbacks
        op_buffers op{
            &write_in_buf_,  &write_in_pos_, &write_in_len_, &write_out_buf_,
            &write_out_len_, false,          false};
        current_op_ = &op;

        for (auto const& buf : buffers)
        {
            char const* src = static_cast<char const*>(buf.data());
            int remaining   = static_cast<int>(buf.size());

            while (remaining > 0)
            {
                op.want_read  = false;
                op.want_write = false;

                int ret = wolfSSL_write(ssl_, src, remaining);

                if (ret > 0)
                {
                    // Successfully wrote some data
                    src += ret;
                    remaining -= ret;
                    total_written += static_cast<std::size_t>(ret);

                    // For write_some semantics, return after first successful write
                    if (total_written > 0)
                    {
                        // Flush any pending output
                        if (write_out_len_ > 0)
                        {
                            auto [lec] = co_await io_cm_.lock();
                            if (lec)
                            {
                                current_op_ = nullptr;
                                co_return {lec, total_written};
                            }
                            capy::async_mutex::lock_guard io_guard(&io_cm_);
                            auto [wec, wn] = co_await capy::write(
                                *s_,
                                capy::const_buffer(
                                    write_out_buf_.data(), write_out_len_));
                            write_out_len_ = 0;
                            if (wec)
                            {
                                current_op_ = nullptr;
                                co_return {wec, total_written};
                            }
                        }
                        current_op_ = nullptr;
                        co_return {std::error_code{}, total_written};
                    }
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if (err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        if (write_out_len_ > 0)
                        {
                            auto [lec] = co_await io_cm_.lock();
                            if (lec)
                            {
                                current_op_ = nullptr;
                                co_return {lec, total_written};
                            }
                            capy::async_mutex::lock_guard io_guard(&io_cm_);
                            auto [wec, wn] = co_await capy::write(
                                *s_,
                                capy::const_buffer(
                                    write_out_buf_.data(), write_out_len_));
                            write_out_len_ = 0;
                            if (wec)
                            {
                                current_op_ = nullptr;
                                co_return {wec, total_written};
                            }
                        }
                    }
                    else if (err == WOLFSSL_ERROR_WANT_READ)
                    {
                        // Renegotiation
                        if (write_in_pos_ == write_in_len_)
                        {
                            write_in_pos_ = 0;
                            write_in_len_ = 0;
                        }
                        capy::mutable_buffer rbuf(
                            write_in_buf_.data() + write_in_len_,
                            write_in_buf_.size() - write_in_len_);
                        auto [lec] = co_await io_cm_.lock();
                        if (lec)
                        {
                            current_op_ = nullptr;
                            co_return {lec, total_written};
                        }
                        capy::async_mutex::lock_guard io_guard(&io_cm_);
                        auto [rec, rn] = co_await s_->read_some(rbuf);
                        if (rec)
                        {
                            current_op_ = nullptr;
                            co_return {rec, total_written};
                        }
                        write_in_len_ += rn;
                    }
                    else
                    {
                        // Other error
                        current_op_ = nullptr;
                        co_return {
                            std::error_code(err, wolfssl_category()),
                            total_written};
                    }
                }
            }
        }

        current_op_ = nullptr;
        co_return {std::error_code{}, total_written};
    }

    capy::io_task<> do_handshake(int type)
    {
        if (used_)
            reset();

        std::error_code ec;

        // Initialize SSL object for the specified role (deferred from construction)
        ec = init_ssl_for_role(type);
        if (ec)
            co_return {ec};

        // Set up operation buffers for callbacks (use read buffers for handshake)
        op_buffers op{&read_in_buf_,  &read_in_pos_,  &read_in_len_,
                      &read_out_buf_, &read_out_len_, false,
                      false};
        current_op_ = &op;

        while (true)
        {
            op.want_read  = false;
            op.want_write = false;

            // Call appropriate handshake function based on type
            int ret;
            if (type == wolfssl_stream::client)
                ret = wolfSSL_connect(ssl_);
            else
                ret = wolfSSL_accept(ssl_);

            if (ret == WOLFSSL_SUCCESS)
            {
                // Handshake completed successfully
                used_ = true;
                capture_alpn();
                // Flush any remaining output
                if (read_out_len_ > 0)
                {
                    auto [lec] = co_await io_cm_.lock();
                    if (lec)
                    {
                        ec = lec;
                        break;
                    }
                    capy::async_mutex::lock_guard io_guard(&io_cm_);
                    auto [wec, wn] = co_await capy::write(
                        *s_,
                        capy::const_buffer(
                            read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if (wec)
                        ec = wec;
                }
                break;
            }
            else
            {
                int err = wolfSSL_get_error(ssl_, ret);

                if (err == WOLFSSL_ERROR_WANT_READ)
                {
                    // Must flush (e.g. ClientHello) before reading ServerHello
                    if (read_out_len_ > 0)
                    {
                        auto [lec] = co_await io_cm_.lock();
                        if (lec)
                        {
                            ec = lec;
                            break;
                        }
                        capy::async_mutex::lock_guard io_guard(&io_cm_);
                        auto [wec, wn] = co_await capy::write(
                            *s_,
                            capy::const_buffer(
                                read_out_buf_.data(), read_out_len_));
                        read_out_len_ = 0;
                        if (wec)
                        {
                            ec = wec;
                            break;
                        }
                    }

                    if (read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer rbuf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto [lec] = co_await io_cm_.lock();
                    if (lec)
                    {
                        ec = lec;
                        break;
                    }
                    capy::async_mutex::lock_guard io_guard(&io_cm_);
                    auto [rec, rn] = co_await s_->read_some(rbuf);
                    if (rec)
                    {
                        ec = rec;
                        break;
                    }
                    read_in_len_ += rn;
                }
                else if (err == WOLFSSL_ERROR_WANT_WRITE)
                {
                    if (read_out_len_ > 0)
                    {
                        auto [lec] = co_await io_cm_.lock();
                        if (lec)
                        {
                            ec = lec;
                            break;
                        }
                        capy::async_mutex::lock_guard io_guard(&io_cm_);
                        auto [wec, wn] = co_await capy::write(
                            *s_,
                            capy::const_buffer(
                                read_out_buf_.data(), read_out_len_));
                        read_out_len_ = 0;
                        if (wec)
                        {
                            ec = wec;
                            break;
                        }
                    }
                }
                else
                {
                    // Other error
                    ec = std::error_code(err, wolfssl_category());
                    break;
                }
            }
        }

        current_op_ = nullptr;
        co_return {ec};
    }

    capy::io_task<> do_shutdown()
    {
        std::error_code ec;

        // Set up operation buffers for callbacks (use read buffers for shutdown)
        op_buffers op{&read_in_buf_,  &read_in_pos_,  &read_in_len_,
                      &read_out_buf_, &read_out_len_, false,
                      false};
        current_op_ = &op;

        while (true)
        {
            op.want_read  = false;
            op.want_write = false;

            int ret = wolfSSL_shutdown(ssl_);
            int err =
                (ret != WOLFSSL_SUCCESS) ? wolfSSL_get_error(ssl_, ret) : 0;

            if (ret == WOLFSSL_SUCCESS)
            {
                // Bidirectional shutdown complete - flush any remaining output
                if (read_out_len_ > 0)
                {
                    auto [lec] = co_await io_cm_.lock();
                    if (lec)
                    {
                        ec = lec;
                        break;
                    }
                    capy::async_mutex::lock_guard io_guard(&io_cm_);
                    auto [wec, wn] = co_await capy::write(
                        *s_,
                        capy::const_buffer(
                            read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if (wec)
                        ec = wec;
                }
                break;
            }
            else if (ret == WOLFSSL_SHUTDOWN_NOT_DONE)
            {
                // First, flush any pending output (sends our close_notify)
                if (read_out_len_ > 0)
                {
                    auto [lec] = co_await io_cm_.lock();
                    if (lec)
                    {
                        ec = lec;
                        break;
                    }
                    capy::async_mutex::lock_guard io_guard(&io_cm_);
                    auto [wec, wn] = co_await capy::write(
                        *s_,
                        capy::const_buffer(
                            read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if (wec)
                        break; // Socket error during shutdown write - acceptable
                }

                // Check what WolfSSL needs next
                if (err == WOLFSSL_ERROR_WANT_READ || err == 0)
                {
                    if (read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer rbuf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto [lec] = co_await io_cm_.lock();
                    if (lec)
                    {
                        ec = lec;
                        break;
                    }
                    capy::async_mutex::lock_guard io_guard(&io_cm_);
                    auto [rec, rn] = co_await s_->read_some(rbuf);
                    if (rec)
                        break; // EOF or socket error during shutdown read - acceptable
                    read_in_len_ += rn;
                }
                else if (err == WOLFSSL_ERROR_WANT_WRITE)
                {
                    // Just need to flush more - already done above, continue loop
                }
                else if (
                    err == WOLFSSL_ERROR_SYSCALL || is_zero_return_error(err))
                {
                    // Socket closed or peer sent close_notify - shutdown complete
                    break;
                }
                else
                {
                    // Unexpected error
                    ec = std::error_code(err, wolfssl_category());
                    break;
                }
            }
            else
            {
                // SSL_FATAL_ERROR or negative return
                ec = std::error_code(err, wolfssl_category());
                break;
            }
        }

        current_op_ = nullptr;
        co_return {ec};
    }

    // Initialization

    std::error_code init_ssl_for_role(int type)
    {
        // Already initialized?
        if (ssl_)
            return {};

        // Get cached native contexts from tls_context
        auto& cd     = detail::get_tls_context_data(ctx_);
        auto* native = detail::get_wolfssl_native_context(cd);
        if (!native)
        {
            return std::error_code(
                wolfSSL_get_error(nullptr, 0), wolfssl_category());
        }

        // A requested configuration could not be applied when the native
        // context was built (inverted protocol window, rejected cipher/
        // version, or an unparseable CRL). Fail closed rather than proceed
        // with weakened or unexpected settings.
        if (native->setup_failed_)
            return std::make_error_code(std::errc::invalid_argument);

        // Select appropriate context based on role
        WOLFSSL_CTX* native_ctx = (type == wolfssl_stream::client)
            ? native->client_ctx_
            : native->server_ctx_;

        if (!native_ctx)
        {
            return std::error_code(
                wolfSSL_get_error(nullptr, 0), wolfssl_category());
        }

        // Create SSL session from the role-specific context
        ssl_ = wolfSSL_new(native_ctx);
        if (!ssl_)
        {
            int err = wolfSSL_get_error(nullptr, 0);
            return std::error_code(err, wolfssl_category());
        }

        // Set custom I/O callbacks
        wolfSSL_SSLSetIORecv(ssl_, &recv_callback);
        wolfSSL_SSLSetIOSend(ssl_, &send_callback);

        // Set this impl as the I/O context
        wolfSSL_SetIOReadCtx(ssl_, this);
        wolfSSL_SetIOWriteCtx(ssl_, this);

        // Verify callback handling.
        if (cd.verify_callback)
        {
#if defined(WOLFSSL_ALWAYS_VERIFY_CB)
            // Capable build: the trampoline and its context data are already
            // installed on the WOLFSSL_CTX (see apply_common_settings); the
            // session inherits them. Nothing to do per-session.
#else
            // This WolfSSL build invokes the verify callback only on failure,
            // never on success, so a callback that tightens verification
            // (e.g. certificate pinning) would silently fail open. Rather
            // than mislead, refuse the connection. Rebuild WolfSSL with
            // WOLFSSL_ALWAYS_VERIFY_CB (e.g. --enable-opensslextra) to use
            // verify callbacks, or omit the callback.
            //
            // Free the session first: leaving ssl_ non-null would let a
            // retried handshake() pass the `if (ssl_) return {}` sentinel and
            // proceed on this unconfigured (and unsupported) session.
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(std::errc::function_not_supported);
#endif
        }

        // ALPN. Both client (offer) and server (candidate list) install
        // the same protocol list; WolfSSL negotiates from it.
        if (!cd.alpn_protocols.empty())
        {
#if defined(HAVE_ALPN)
            // FAILED_ON_MISMATCH: on no shared protocol the server aborts the
            // handshake with a fatal no_application_protocol alert (RFC 7301
            // §3.2), matching the OpenSSL backend. A non-success return means
            // the offer was not installed; fail closed rather than proceed.
            if (wolfSSL_UseALPN(
                    ssl_, native->alpn_list_.data(),
                    static_cast<unsigned int>(native->alpn_list_.size()),
                    WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS)
                return std::make_error_code(std::errc::invalid_argument);
#else
            // This WolfSSL build cannot negotiate ALPN. An application that
            // offered protocols (and may read alpn_protocol() expecting a
            // result) must not silently proceed with none; fail closed.
            // Rebuild WolfSSL with HAVE_ALPN to enable ALPN.
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(std::errc::function_not_supported);
#endif
        }

#if !defined(HAVE_CRL)
        // Revocation via CRL requires a WolfSSL build with HAVE_CRL. When a
        // policy actually requests checking, fail closed rather than skip it
        // silently. Gate on the policy alone: a CRL supplied while the policy
        // stays disabled is inert by contract (consulted only when the policy
        // is not disabled), so that config must work here as it does
        // everywhere else.
        if (cd.revocation != tls_revocation_policy::disabled)
        {
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(std::errc::function_not_supported);
        }
#endif

        // Apply per-session config (SNI + hostname verification) from context
        if (type == wolfssl_stream::client && !cd.hostname.empty())
        {
            // Set SNI extension so server knows which cert to present
            wolfSSL_UseSNI(
                ssl_, WOLFSSL_SNI_HOST_NAME, cd.hostname.data(),
                static_cast<unsigned short>(cd.hostname.size()));

            // Enable hostname verification (checks CN/SAN in peer cert)
            wolfSSL_check_domain_name(ssl_, cd.hostname.c_str());
        }

        return {};
    }
};

wolfssl_stream::impl*
wolfssl_stream::make_impl(capy::any_stream& stream, tls_context const& ctx)
{
    // SSL object creation is deferred to handshake time when we know the role
    return new impl(stream, ctx);
}

wolfssl_stream::~wolfssl_stream()
{
    delete impl_;
}

wolfssl_stream::wolfssl_stream(wolfssl_stream&& other) noexcept
    : stream_(std::move(other.stream_))
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
    if (impl_)
        impl_->s_ = &stream_;
}

wolfssl_stream&
wolfssl_stream::operator=(wolfssl_stream&& other) noexcept
{
    if (this != &other)
    {
        delete impl_;
        stream_     = std::move(other.stream_);
        impl_       = other.impl_;
        other.impl_ = nullptr;
        if (impl_)
            impl_->s_ = &stream_;
    }
    return *this;
}

capy::io_task<std::size_t>
wolfssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_read_some(buffers);
}

capy::io_task<std::size_t>
wolfssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_write_some(buffers);
}

capy::io_task<>
wolfssl_stream::handshake(handshake_type type)
{
    co_return co_await impl_->do_handshake(type);
}

capy::io_task<>
wolfssl_stream::shutdown()
{
    co_return co_await impl_->do_shutdown();
}

void
wolfssl_stream::reset()
{
    impl_->reset();
}

std::string_view
wolfssl_stream::name() const noexcept
{
    return "wolfssl";
}

std::string_view
wolfssl_stream::alpn_protocol() const noexcept
{
    return impl_->alpn_selected_;
}

} // namespace boost::corosio
