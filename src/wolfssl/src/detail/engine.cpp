//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "engine.hpp"

// wolfssl_category and the capability queries are declared alongside
// the public stream class
#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/capy/error.hpp>

// Internal context implementation
#include "src/tls/detail/context_impl.hpp"

// WolfSSL options must precede every other WolfSSL header so feature
// macros match the linked library.
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/pkcs12.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace boost::corosio {

namespace {

// Large enough to hold the largest possible TLS record.
constexpr std::size_t staging_size = std::size_t{17} * 1024;

inline bool
is_zero_return_error(int err) noexcept
{
    return err == WOLFSSL_ERROR_ZERO_RETURN;
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

bool
wolfssl_supports_ip_alt_name() noexcept
{
#if defined(OPENSSL_EXTRA) && defined(WOLFSSL_IP_ALT_NAME)
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
// closed. This mirrors the soft_fail contract: tolerate "status unknown",
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
// callback is refused entirely (see engine::init).
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
    // Reason the requested configuration could not be applied, latched
    // from the first failure so engine::init fails closed rather than
    // negotiate an unexpected version, ignore the requested ciphers,
    // weaken revocation (fail-open under soft_fail), or hand out a
    // handshake with an empty trust store. Zero means success; a negative
    // value is the library's own error code (e.g. a trust anchor that
    // would not load, which init surfaces so it is not misread as a
    // downstream ASN_NO_SIGNER_E); a positive value marks a config
    // failure with no library code (inverted window, rejected cipher).
    int setup_error_ = 0;
    // ALPN protocol list in WolfSSL's comma-separated format, built once
    // from the immutable protocol list. Installed per-session (client offer
    // / server candidates); caching it avoids rebuilding per connection.
    std::string alpn_list_;

    void
    apply_common_settings(WOLFSSL_CTX* ctx, tls_context_data const& cd)
    {
        if (!ctx)
            return;

        int verify_mode_flag = WOLFSSL_VERIFY_NONE;
        if (cd.verification_mode == tls_verify_mode::peer)
            verify_mode_flag = WOLFSSL_VERIFY_PEER;
        else if (cd.verification_mode == tls_verify_mode::require_peer)
            verify_mode_flag =
                WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;

        // On capable builds install the verify trampoline and stash the
        // context data in the WOLFSSL_CTX ex_data, where the trampoline
        // recovers it. On builds that cannot honor the callback the
        // handshake is refused in engine::init instead.
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
        // (see below).
        if (!cd.pkcs12_data.empty())
        {
            // A bundle that fails to decode or parse (wrong passphrase,
            // malformed) must not leave the context silently credential-less:
            // a client using PKCS#12 for mTLS would then fail open against a
            // verify_mode::peer server. Fail closed like every other setup
            // error.
            WC_PKCS12* p12 = wc_PKCS12_new();
            if (!p12)
                setup_error_ = setup_error_ ? setup_error_ : 1;
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
                    setup_error_ = setup_error_ ? setup_error_ : 1;
                }
                else
                {
                    if (cert && ca)
                    {
                        // Concatenate leaf + chain into one DER blob and load
                        // it as a chain so the intermediate(s) are sent during
                        // the handshake.
                        std::vector<unsigned char> chain(cert, cert + certSz);
                        for (WC_DerCertList* n = ca; n; n = n->next)
                            chain.insert(
                                chain.end(), n->buffer, n->buffer + n->bufferSz);
                        if (wolfSSL_CTX_use_certificate_chain_buffer_format(
                                ctx, chain.data(),
                                static_cast<long>(chain.size()),
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_error_ = setup_error_ ? setup_error_ : 1;
                    }
                    else if (cert)
                    {
                        if (wolfSSL_CTX_use_certificate_buffer(
                                ctx, cert, static_cast<long>(certSz),
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_error_ = setup_error_ ? setup_error_ : 1;
                    }
                    if (pkey &&
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx, pkey, static_cast<long>(pkeySz),
                            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                        setup_error_ = setup_error_ ? setup_error_ : 1;
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
        // the credential above, so skip them.
        // An entity credential that fails to load must not pass silently:
        // the handshake would run without the identity the caller
        // configured and fail remotely instead of at setup.
        if (cd.pkcs12_data.empty() && !cd.certificate_chain.empty())
        {
            if (wolfSSL_CTX_use_certificate_chain_buffer(
                    ctx,
                    reinterpret_cast<unsigned char const*>(
                        cd.certificate_chain.data()),
                    static_cast<long>(cd.certificate_chain.size())) !=
                WOLFSSL_SUCCESS)
                setup_error_ = setup_error_ ? setup_error_ : 1;
        }
        else if (cd.pkcs12_data.empty() && !cd.entity_certificate.empty())
        {
            // Only use single certificate if no chain provided
            int format = (cd.entity_cert_format == tls_file_format::pem)
                ? WOLFSSL_FILETYPE_PEM
                : WOLFSSL_FILETYPE_ASN1;
            if (wolfSSL_CTX_use_certificate_buffer(
                    ctx,
                    reinterpret_cast<unsigned char const*>(
                        cd.entity_certificate.data()),
                    static_cast<long>(cd.entity_certificate.size()),
                    format) != WOLFSSL_SUCCESS)
                setup_error_ = setup_error_ ? setup_error_ : 1;
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

                    if (der_len <= 0 ||
                        wolfSSL_CTX_use_PrivateKey_buffer(
                            ctx, der_buf.data(), der_len,
                            WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                        setup_error_ = setup_error_ ? setup_error_ : 1;
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
                    {
                        if (wolfSSL_CTX_use_PrivateKey_buffer(
                                ctx, der_buf.data(), dec_len,
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_error_ = setup_error_ ? setup_error_ : 1;
                    }
                    else
                    {
                        // Not encrypted or decryption failed - try loading directly
                        if (wolfSSL_CTX_use_PrivateKey_buffer(
                                ctx,
                                reinterpret_cast<unsigned char const*>(
                                    cd.private_key.data()),
                                static_cast<long>(cd.private_key.size()),
                                WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS)
                            setup_error_ = setup_error_ ? setup_error_ : 1;
                    }
                }
            }
            else
            {
                int format = (cd.private_key_format == tls_file_format::pem)
                    ? WOLFSSL_FILETYPE_PEM
                    : WOLFSSL_FILETYPE_ASN1;
                if (wolfSSL_CTX_use_PrivateKey_buffer(
                        ctx,
                        reinterpret_cast<unsigned char const*>(
                            cd.private_key.data()),
                        static_cast<long>(cd.private_key.size()), format) !=
                    WOLFSSL_SUCCESS)
                    setup_error_ = setup_error_ ? setup_error_ : 1;
            }
        }

        for (auto const& ca : cd.ca_certificates)
        {
            // A trust anchor that fails to load must not pass silently: the
            // context would then verify against an empty store and every
            // handshake would fail ASN_NO_SIGNER_E. Fail closed and keep the
            // library's reason so init reports it directly.
            int const ret = wolfSSL_CTX_load_verify_buffer(
                ctx, reinterpret_cast<unsigned char const*>(ca.data()),
                static_cast<long>(ca.size()), WOLFSSL_FILETYPE_PEM);
            if (ret != WOLFSSL_SUCCESS)
                setup_error_ = setup_error_ ? setup_error_ : ret;
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
            setup_error_ = setup_error_ ? setup_error_ : 1;
        if (wolfSSL_CTX_SetMinVersion(
                ctx, wolfssl_min_version_const(cd.min_version)) !=
            WOLFSSL_SUCCESS)
            setup_error_ = setup_error_ ? setup_error_ : 1;

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
                setup_error_ = setup_error_ ? setup_error_ : 1;
        }

#if defined(HAVE_CRL)
        // Certificate revocation via CRLs (fuller builds only; when HAVE_CRL
        // is absent, engine::init fails closed instead).
        if (cd.revocation != tls_revocation_policy::disabled)
        {
            if (wolfSSL_CTX_EnableCRL(ctx, WOLFSSL_CRL_CHECK) !=
                WOLFSSL_SUCCESS)
                setup_error_ = setup_error_ ? setup_error_ : 1;
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
                    setup_error_ = setup_error_ ? setup_error_ : 1;
            }
            // soft_fail tolerates an undeterminable revocation status (no CRL
            // loaded for a cert). Without this, WolfSSL's WOLFSSL_CRL_CHECK
            // hard-fails with CRL_MISSING; the callback downgrades that to
            // success while still rejecting a cert that a loaded CRL actually
            // revokes.
            if (cd.revocation == tls_revocation_policy::soft_fail)
                wolfSSL_CTX_SetCRL_ErrorCb(
                    ctx, &detail::wolfssl_crl_soft_fail_cb, nullptr);
        }
#endif
    }

    explicit wolfssl_native_context(tls_context_data const& cd)
        : client_ctx_(nullptr)
        , server_ctx_(nullptr)
    {
        // Create separate contexts for client and server, choosing the
        // method that honors the configured protocol version window.
        client_ctx_ = wolfSSL_CTX_new(
            wolfssl_method_for(false, cd.min_version, cd.max_version));
        server_ctx_ = wolfSSL_CTX_new(
            wolfssl_method_for(true, cd.min_version, cd.max_version));

        // The construction below can throw (std::string/vector
        // allocation); a constructor exception skips this object's own
        // destructor, so the two raw WOLFSSL_CTX* would otherwise leak.
        try
        {
            apply_common_settings(client_ctx_, cd);
            apply_common_settings(server_ctx_, cd);

            // Encode the ALPN protocol list once (comma-separated); each
            // session installs it via wolfSSL_UseALPN in engine::init.
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
        catch (...)
        {
            if (client_ctx_)
                wolfSSL_CTX_free(client_ctx_);
            if (server_ctx_)
                wolfSSL_CTX_free(server_ctx_);
            throw;
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

//
// engine
//

namespace wolfssl {

engine::~engine()
{
    if (ssl_)
        wolfSSL_free(ssl_);
    // WOLFSSL_CTX* is owned by the cached native context, not freed here
}

engine::engine()
{
    in_.resize(staging_size);
    out_.resize(staging_size);
}

int
engine::recv_callback(WOLFSSL*, char* buf, int sz, void* ctx)
{
    auto* self = static_cast<engine*>(ctx);

    std::size_t available = self->in_len_ - self->in_pos_;
    if (available == 0)
        return WOLFSSL_CBIO_ERR_WANT_READ;

    std::size_t to_copy =
        (std::min)(available, static_cast<std::size_t>(sz));
    std::memcpy(buf, self->in_.data() + self->in_pos_, to_copy);
    self->in_pos_ += to_copy;

    if (self->in_pos_ == self->in_len_)
    {
        self->in_pos_ = 0;
        self->in_len_ = 0;
    }
    return static_cast<int>(to_copy);
}

int
engine::send_callback(WOLFSSL*, char* buf, int sz, void* ctx)
{
    auto* self = static_cast<engine*>(ctx);

    std::size_t available = self->out_.size() - self->out_len_;
    if (available == 0)
        return WOLFSSL_CBIO_ERR_WANT_WRITE;

    std::size_t to_copy =
        (std::min)(available, static_cast<std::size_t>(sz));
    std::memcpy(self->out_.data() + self->out_len_, buf, to_copy);
    self->out_len_ += to_copy;

    return static_cast<int>(to_copy);
}

std::error_code
engine::init(tls_context const& ctx, tls_role role, std::string const& hostname)
{
    if (ssl_)
        return {};

    auto& cd     = get_tls_context_data(ctx);
    auto* native = get_wolfssl_native_context(cd);
    if (!native)
    {
        return std::error_code(
            wolfSSL_get_error(nullptr, 0), wolfssl_category());
    }

    // A requested configuration could not be applied when the native
    // context was built (inverted protocol window, rejected cipher/
    // version, an unparseable CRL, or a trust anchor that would not load).
    // Fail closed rather than proceed with weakened or unexpected settings;
    // a negative code is the library's own reason (surfaced so a failed
    // trust load is not misread as a downstream verify miss), a positive
    // marker is a config failure with no library code.
    if (native->setup_error_ != 0)
        return native->setup_error_ < 0
            ? std::error_code(native->setup_error_, wolfssl_category())
            : std::make_error_code(std::errc::invalid_argument);

    WOLFSSL_CTX* native_ctx = (role == tls_role::client)
        ? native->client_ctx_
        : native->server_ctx_;

    if (!native_ctx)
    {
        return std::error_code(
            wolfSSL_get_error(nullptr, 0), wolfssl_category());
    }

    ssl_ = wolfSSL_new(native_ctx);
    if (!ssl_)
    {
        int err = wolfSSL_get_error(nullptr, 0);
        return std::error_code(err, wolfssl_category());
    }

    // Route all session I/O through the engine's staging buffers
    wolfSSL_SSLSetIORecv(ssl_, &engine::recv_callback);
    wolfSSL_SSLSetIOSend(ssl_, &engine::send_callback);
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
        // §3.2). A non-success return means the offer was not installed;
        // fail closed rather than proceed.
        if (wolfSSL_UseALPN(
                ssl_, native->alpn_list_.data(),
                static_cast<unsigned int>(native->alpn_list_.size()),
                WOLFSSL_ALPN_FAILED_ON_MISMATCH) != WOLFSSL_SUCCESS)
        {
            // Leaving ssl_ non-null would let a retried handshake()
            // pass the `if (ssl_) return {}` sentinel and proceed on
            // this half-configured session (every sibling failure
            // branch in this function frees and nulls it).
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(std::errc::invalid_argument);
        }
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

    // Apply per-session config (SNI + hostname verification)
    if (role == tls_role::client && !hostname.empty())
    {
        if (is_ip_literal(hostname))
        {
            // RFC 6066 excludes IP literals from SNI; the literal
            // must match the certificate's iPAddress entries.
            // Enforcement needs both flags: OPENSSL_EXTRA routes
            // the address into the verify params that the cert
            // check consults, and WOLFSSL_IP_ALT_NAME makes the
            // parser record iPAddress entries at all.
#if defined(OPENSSL_EXTRA) && defined(WOLFSSL_IP_ALT_NAME)
            // Install via the verify params directly, not
            // wolfSSL_check_ip_address: that wrapper reports
            // success even when enforcement is compiled out, and
            // binding these OPENSSL_EXTRA-only symbols makes a
            // run against a downgraded WolfSSL fail at load
            // rather than skip the check silently.
            WOLFSSL_X509_VERIFY_PARAM* vp = wolfSSL_get0_param(ssl_);
            if (!vp ||
                wolfSSL_X509_VERIFY_PARAM_set1_ip_asc(
                    vp, hostname.c_str()) != WOLFSSL_SUCCESS)
            {
                // Fail closed rather than handshake without the
                // requested name check.
                wolfSSL_free(ssl_);
                ssl_ = nullptr;
                return std::make_error_code(
                    std::errc::invalid_argument);
            }
#else
            wolfSSL_free(ssl_);
            ssl_ = nullptr;
            return std::make_error_code(
                std::errc::function_not_supported);
#endif
        }
        else
        {
            // Set SNI extension so server knows which cert to present
            int ret = wolfSSL_UseSNI(
                ssl_, WOLFSSL_SNI_HOST_NAME, hostname.data(),
                static_cast<unsigned short>(hostname.size()));

            // Enable hostname verification (checks CN/SAN in peer cert)
            if (ret == WOLFSSL_SUCCESS)
                ret = wolfSSL_check_domain_name(ssl_, hostname.c_str());

            if (ret != WOLFSSL_SUCCESS)
            {
                // Fail closed rather than handshake without the
                // requested name check.
                wolfSSL_free(ssl_);
                ssl_ = nullptr;
                return std::make_error_code(std::errc::invalid_argument);
            }
        }
    }

    return {};
}

void
engine::reset()
{
    // The session is released rather than cleared: the next init
    // rebuilds it from the (possibly different) role's context, so a
    // rechanged hostname or role is honored. Staging allocations kept.
    if (ssl_)
    {
        wolfSSL_free(ssl_);
        ssl_ = nullptr;
    }
    in_pos_ = 0;
    in_len_ = 0;
    out_len_ = 0;
    received_close_notify_ = false;
}

void
engine::capture_alpn(std::string& out) const
{
#if defined(HAVE_ALPN)
    char* name        = nullptr;
    unsigned short sz = 0;
    if (wolfSSL_ALPN_GetProtocol(ssl_, &name, &sz) == WOLFSSL_SUCCESS &&
        name && sz)
        out.assign(name, sz);
#else
    (void)out;
#endif
}

engine_result
engine::perform(engine_op op, void* data, std::size_t len)
{
    int ret = 0;
    switch (op)
    {
    case engine_op::handshake_client:
        ret = wolfSSL_connect(ssl_);
        break;
    case engine_op::handshake_server:
        ret = wolfSSL_accept(ssl_);
        break;
    case engine_op::read:
        ret = wolfSSL_read(ssl_, data, static_cast<int>(len));
        break;
    case engine_op::write:
        ret = wolfSSL_write(ssl_, data, static_cast<int>(len));
        break;
    case engine_op::shutdown:
        // Both close_notifies already exchanged: the bidirectional close
        // is complete. Re-calling wolfSSL_shutdown on some builds
        // re-initiates the close (clearing RECEIVED_SHUTDOWN and re-queuing
        // a close_notify), then blocks forever awaiting a close_notify the
        // completed peer never resends. Requiring the SENT bit too is
        // essential: a peer that has only *received* a close_notify (e.g.
        // drained it from a prior read) must still send its own, so this
        // must not short-circuit before wolfSSL_shutdown queues it.
        if ((wolfSSL_get_shutdown(ssl_) &
             (WOLFSSL_SENT_SHUTDOWN | WOLFSSL_RECEIVED_SHUTDOWN)) ==
            (WOLFSSL_SENT_SHUTDOWN | WOLFSSL_RECEIVED_SHUTDOWN))
            // A close_notify staged by an earlier re-query still needs to
            // reach the peer, so keep the flush verdict rather than a bare
            // done when output remains.
            return {
                pending_output() > 0 ? engine_want::output_then_done
                                     : engine_want::done,
                {}, 0};
        ret = wolfSSL_shutdown(ssl_);
        break;
    }

    // Some wolfSSL builds clear the shutdown bitmask on a read that
    // follows a completed close, so latch the peer's close_notify the
    // moment it is first observed; every later read must still report end
    // of stream even after the session has forgotten it.
    if (wolfSSL_get_shutdown(ssl_) & WOLFSSL_RECEIVED_SHUTDOWN)
        received_close_notify_ = true;

    if (op == engine_op::shutdown)
    {
        // Shutdown has its own dispatch: a completed bidirectional
        // close and the waiting-for-close_notify state use dedicated
        // return values rather than the error channel.
        if (ret == WOLFSSL_SUCCESS)
            return {
                pending_output() > 0 ? engine_want::output_then_done
                                     : engine_want::done,
                {}, 0};

        // Once the peer's close_notify is latched the bidirectional
        // close is complete, so never park for more input: some
        // wolfSSL builds report SHUTDOWN_NOT_DONE or WANT_READ from a
        // second shutdown on an already-closed session, which would
        // otherwise read the transport for bytes that never come.
        if (received_shutdown())
            return {
                pending_output() > 0 ? engine_want::output_then_done
                                     : engine_want::done,
                {}, 0};

        // Our close_notify was queued but the peer's has not arrived
        // yet: flush it, then read for it.
        if (ret == WOLFSSL_SHUTDOWN_NOT_DONE)
            return {
                pending_output() > 0 ? engine_want::output_then_retry
                                     : engine_want::input,
                {}, 0};

        int const err = wolfSSL_get_error(ssl_, ret);

        // A shutdown queried with nothing staged reports WANT_READ
        // while it awaits the peer's close_notify; the retry protocol
        // reaches this state (it re-runs the shutdown after a flush,
        // before new input arrives), so route it to the same
        // flush-then-fill treatment as SHUTDOWN_NOT_DONE.
        if (err == WOLFSSL_ERROR_WANT_WRITE)
            return {engine_want::output_then_retry, {}, 0};

        if (err == WOLFSSL_ERROR_WANT_READ)
            return {
                pending_output() > 0 ? engine_want::output_then_retry
                                     : engine_want::input,
                {}, 0};

        // A close_notify that races a concurrently parked read
        // surfaces here (ret == WOLFSSL_FATAL_ERROR) rather than
        // through SHUTDOWN_NOT_DONE, but it is still a completed
        // bidirectional shutdown, not an error.
        if (is_zero_return_error(err))
            return {
                pending_output() > 0 ? engine_want::output_then_done
                                     : engine_want::done,
                {}, 0};

        return {
            pending_output() > 0 ? engine_want::output_then_done
                                 : engine_want::done,
            std::error_code(err, wolfssl_category()), 0};
    }

    bool const transfer = op == engine_op::read || op == engine_op::write;

    if (transfer ? ret > 0 : ret == WOLFSSL_SUCCESS)
        return {
            pending_output() > 0 ? engine_want::output_then_done
                                 : engine_want::done,
            {}, transfer ? static_cast<std::size_t>(ret) : 0};

    int const err = wolfSSL_get_error(ssl_, ret);

    if (err == WOLFSSL_ERROR_WANT_WRITE)
        return {engine_want::output_then_retry, {}, 0};

    if (err == WOLFSSL_ERROR_WANT_READ)
    {
        // A read issued after the peer's close_notify was already
        // received must report end of stream, not park on the transport
        // for bytes that will never arrive. wolfSSL surfaces this as
        // WANT_READ (it does not fold an already-received peer shutdown
        // into a zero-return the way an in-band close_notify does), so
        // consult the latched close state. The live bitmask cannot be
        // used here: some builds clear it on the very read that reaches
        // this branch, which is exactly why the latch exists.
        if (op == engine_op::read && received_close_notify_)
            return {
                engine_want::done, make_error_code(capy::error::eof), 0};

        // A write cannot make progress once the peer's close_notify has
        // been latched: the connection is closed, so parking on transport
        // input would wait for bytes that never arrive. Some builds accept
        // an application write after a completed close (queuing the byte);
        // others surface it as WANT_READ. Report the closed stream rather
        // than hang, flushing any control record the failed write queued.
        if (op == engine_op::write && received_close_notify_)
            return {
                pending_output() > 0 ? engine_want::output_then_done
                                     : engine_want::done,
                make_error_code(capy::error::eof), 0};

        return {
            pending_output() > 0 ? engine_want::output_then_retry
                                 : engine_want::input,
            {}, 0};
    }

    if (transfer && is_zero_return_error(err))
    {
        // A received close_notify is an announced close: report eof,
        // unlike the terminal branch below, which surfaces the raw
        // library code. It queues no output, so a plain done skips
        // the flush.
        return {engine_want::done, make_error_code(capy::error::eof), 0};
    }

    // Terminal (handshake ZERO_RETURN included): surface the library
    // code directly.
    return {
        pending_output() > 0 ? engine_want::output_then_done
                             : engine_want::done,
        std::error_code(err, wolfssl_category()), 0};
}

std::size_t
engine::put_input(unsigned char const* data, std::size_t len)
{
    if (in_pos_ == in_len_)
    {
        in_pos_ = 0;
        in_len_ = 0;
    }
    // No mid-data compaction: the recv callback resets the cursors
    // whenever it drains the staging, and an input verdict implies it
    // did, so deposits effectively always start at the front.
    std::size_t const n = (std::min)(in_.size() - in_len_, len);
    std::memcpy(in_.data() + in_len_, data, n);
    in_len_ += n;
    return n;
}

std::pair<unsigned char*, std::size_t>
engine::input_area()
{
    // Compact any partially consumed leftover to the front so the whole
    // remaining capacity is one contiguous run the transport can read
    // ciphertext directly into (no staging copy).
    if (in_pos_ == in_len_)
    {
        in_pos_ = 0;
        in_len_ = 0;
    }
    else if (in_pos_ > 0)
    {
        std::memmove(in_.data(), in_.data() + in_pos_, in_len_ - in_pos_);
        in_len_ -= in_pos_;
        in_pos_ = 0;
    }
    return {
        reinterpret_cast<unsigned char*>(in_.data()) + in_len_,
        in_.size() - in_len_};
}

void
engine::input_committed(std::size_t n)
{
    in_len_ += n;
}

std::size_t
engine::pending_output() const
{
    return out_len_;
}

std::size_t
engine::get_output(unsigned char* data, std::size_t len)
{
    std::size_t const n = (std::min)(len, out_len_);
    std::memcpy(data, out_.data(), n);
    std::memmove(out_.data(), out_.data() + n, out_len_ - n);
    out_len_ -= n;
    return n;
}

bool
engine::received_shutdown() const
{
    // WOLFSSL_SENT_SHUTDOWN is excluded: it latches as soon as our own
    // close_notify is queued, which happens on every local shutdown()
    // call regardless of what the peer ever sends. Only the received
    // bit reflects whether the peer's close_notify actually arrived.
    return (wolfSSL_get_shutdown(ssl_) & WOLFSSL_RECEIVED_SHUTDOWN) != 0;
}

} // namespace wolfssl

} // namespace detail

} // namespace boost::corosio
