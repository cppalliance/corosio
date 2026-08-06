//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "engine.hpp"

// openssl_category is declared alongside the public stream class
#include <boost/corosio/openssl_stream.hpp>
#include <boost/capy/error.hpp>

// Internal context implementation
#include "src/tls/detail/context_impl.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/pkcs12.h>

#include <cstring>
#include <vector>

namespace boost::corosio {

namespace {

inline SSL_METHOD const*
tls_method_compat() noexcept
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    return TLS_method();
#else
    return SSLv23_method();
#endif
}

inline bool
apply_hostname_verification(SSL* ssl, std::string const& hostname)
{
    // SSL_clear retains a previously applied name; an empty hostname
    // must clear SNI and the verify-param host or a reset stream
    // would leak the old peer's name into the next handshake
    char const* name = hostname.empty() ? nullptr : hostname.c_str();

    // RFC 6066 excludes IP literals from SNI, and a literal must match
    // the certificate's iPAddress entries rather than its DNS names.
    // The unused field is cleared so a reset stream cannot carry the
    // previous target's matching rule.
    bool const is_ip     = name && detail::is_ip_literal(hostname);
    char const* dns_name = is_ip ? nullptr : name;

    if (SSL_set_tlsext_host_name(ssl, dns_name) != 1)
        return false;

    auto* param = SSL_get0_param(ssl);
    if (!param)
        return name == nullptr;

    if (X509_VERIFY_PARAM_set1_host(param, dns_name, 0) != 1)
        return false;
    if (is_ip)
        return X509_VERIFY_PARAM_set1_ip_asc(param, name) == 1;
    return X509_VERIFY_PARAM_set1_ip(param, nullptr, 0) == 1;
}

// Map a portable protocol version to the OpenSSL version constant.
inline int
openssl_proto_version(tls_version v) noexcept
{
    return v == tls_version::tls_1_3 ? TLS1_3_VERSION : TLS1_2_VERSION;
}

// Encode a protocol list into ALPN wire format: each entry is a
// one-byte length followed by that many bytes. Entries longer than 255
// bytes are skipped (invalid per RFC 7301).
inline std::string
build_alpn_wire(std::vector<std::string> const& protocols)
{
    std::string wire;
    for (auto const& p : protocols)
    {
        if (p.empty() || p.size() > 255)
            continue;
        wire.push_back(static_cast<char>(p.size()));
        wire.append(p);
    }
    return wire;
}

class openssl_category_impl final : public std::error_category
{
    char const*
    name() const noexcept override
    {
        return "corosio.openssl";
    }

    std::string
    message(int value) const override
    {
        char buf[256];
        ::ERR_error_string_n(
            static_cast<unsigned long>(value), buf, sizeof(buf));
        return buf;
    }
};

// Convert a packed OpenSSL error (from ERR_get_error) into an error_code.
// Codes from the ERR_LIB_SYS library carry a genuine errno reason and are
// reported with the system category; everything else uses openssl_category.
inline std::error_code
make_openssl_error(unsigned long err) noexcept
{
    if (ERR_GET_LIB(err) == ERR_LIB_SYS)
        return std::error_code(
            static_cast<int>(ERR_GET_REASON(err)), std::system_category());
    return std::error_code(static_cast<int>(err), openssl_category());
}

// Maps a terminal engine failure (any path that isn't WANT_READ/WANT_WRITE/
// ZERO_RETURN) to an error_code. SYSCALL and a bare SSL_ERROR_SSL both leave
// the queue empty when the transport simply vanished rather than reporting
// a protocol failure; make_openssl_error(0) is falsy, which would otherwise
// turn that vanished transport into a false success, so an empty queue maps
// to stream_truncated instead.
inline std::error_code
map_openssl_fatal() noexcept
{
    unsigned long ssl_err = ERR_get_error();
    if (ssl_err == 0)
        return make_error_code(capy::error::stream_truncated);
    return make_openssl_error(ssl_err);
}

} // namespace

std::error_category const&
openssl_category() noexcept
{
    static openssl_category_impl instance;
    return instance;
}

//
// Native context caching
//

namespace detail {

static int sni_ctx_data_index = -1;

static int
password_callback(char* buf, int size, int rwflag, void* userdata)
{
    auto* cd = static_cast<tls_context_data const*>(userdata);
    if (!cd || !cd->password_callback)
        return 0;

    tls_password_purpose purpose = (rwflag == 0)
        ? tls_password_purpose::for_reading
        : tls_password_purpose::for_writing;

    std::string password =
        cd->password_callback(static_cast<std::size_t>(size), purpose);

    int len = static_cast<int>(password.size());
    if (len > size)
        len = size;

    std::memcpy(buf, password.data(), static_cast<std::size_t>(len));
    return len;
}

// Trampoline installed via SSL_CTX_set_verify. Recovers the portable
// context data from the SSL_CTX ex_data (populated for every context)
// and applies, in order: the revocation policy's soft-fail downgrade,
// then the user's verify callback. Installed whenever a verify callback
// or a non-disabled revocation policy is configured.
static int
verify_callback_trampoline(int preverified, X509_STORE_CTX* store_ctx)
{
    SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
        store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl)
        return preverified;

    auto* cd = static_cast<tls_context_data const*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), sni_ctx_data_index));
    if (!cd)
        return preverified;

    bool ok = preverified != 0;

    // Soft-fail revocation: accept certificates whose revocation status
    // could not be determined (missing/expired CRL), but never downgrade
    // an actual revocation. hard_fail leaves every CRL error fatal.
    if (!ok && cd->revocation == tls_revocation_policy::soft_fail)
    {
        int const err = X509_STORE_CTX_get_error(store_ctx);
        if (err == X509_V_ERR_UNABLE_TO_GET_CRL ||
            err == X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER ||
            err == X509_V_ERR_CRL_HAS_EXPIRED ||
            err == X509_V_ERR_CRL_NOT_YET_VALID)
            ok = true;
    }

    if (cd->verify_callback)
    {
        // Expose the current certificate's DER so the callback can inspect
        // it portably. i2d_X509 allocates; free it after the callback.
        X509* cert         = X509_STORE_CTX_get_current_cert(store_ctx);
        unsigned char* der = nullptr;
        int der_len        = cert ? i2d_X509(cert, &der) : 0;

        verify_context vc(
            store_ctx, der,
            der_len > 0 ? static_cast<std::size_t>(der_len) : 0);
        ok = cd->verify_callback(ok, vc);

        if (der)
            OPENSSL_free(der);
    }

    return ok ? 1 : 0;
}

// Server-side ALPN selection. Chooses the server's most-preferred
// protocol that the client also offered. On no overlap it sends a fatal
// no_application_protocol alert (RFC 7301 §3.2).
//
// `arg` points at the native context's build-time snapshot of the server
// preference list (a std::vector<std::string>), so client offer and server
// selection are both taken from the same immutable snapshot.
//
// The selected protocol pointer must stay valid until the callback runs
// again, so we point *out into the client list `in` (OpenSSL keeps it
// valid for the connection) rather than into a local buffer.
static int
alpn_select_cb(
    SSL* /* ssl */, unsigned char const** out, unsigned char* outlen,
    unsigned char const* in, unsigned int inlen, void* arg)
{
    auto const* prefs = static_cast<std::vector<std::string> const*>(arg);
    if (!prefs || prefs->empty())
        return SSL_TLSEXT_ERR_NOACK; // nothing configured (defensive)

    // Server preference order wins: for each server protocol, look for a
    // matching entry in the client's offered list.
    for (auto const& pref : *prefs)
    {
        for (unsigned int i = 0; i + 1 <= inlen;)
        {
            unsigned int len = in[i];
            if (i + 1 + len > inlen)
                break; // malformed
            if (len == pref.size() &&
                std::memcmp(in + i + 1, pref.data(), len) == 0)
            {
                *out    = in + i + 1;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            i += 1 + len;
        }
    }

    // The server supports ALPN but shares no protocol with the client.
    // RFC 7301 §3.2: fail the handshake with a fatal alert.
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static int
sni_callback(SSL* ssl, int* /* alert */, void* /* arg */)
{
    char const* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!servername)
        return SSL_TLSEXT_ERR_NOACK;

    SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
    auto* cd     = static_cast<tls_context_data const*>(
        SSL_CTX_get_ex_data(ctx, sni_ctx_data_index));

    if (cd && cd->servername_callback)
    {
        if (!cd->servername_callback(servername))
            return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    return SSL_TLSEXT_ERR_OK;
}

class openssl_native_context : public native_context_base
{
public:
    SSL_CTX* ctx_;
    tls_context_data const* cd_;
    // Set when a requested configuration could not be applied: an inverted
    // protocol window (min > max), a cipher list / suite the library
    // rejected, a protocol-version bound that would not set, or a CRL that
    // parsed as neither PEM nor DER. Silently proceeding would negotiate an
    // unexpected version, ignore the requested ciphers, or weaken revocation
    // (fail-open under soft_fail), so do_handshake refuses the handshake.
    bool setup_failed_ = false;
    // ALPN offer in wire format (length-prefixed), encoded once from the
    // immutable protocol list. The client sets it per-SSL each handshake;
    // caching it here avoids re-encoding and re-allocating per connection.
    std::string alpn_wire_;
    // Server preference snapshot, captured at build time so the select
    // callback matches against the same immutable list the client offers
    // from (see alpn_select_cb). Its address is handed to OpenSSL as the
    // callback arg, so it must outlive the SSL_CTX (it does — same object).
    std::vector<std::string> alpn_snapshot_;

    explicit openssl_native_context(tls_context_data const& cd)
        : ctx_(nullptr)
        , cd_(&cd)
    {
        ctx_ = SSL_CTX_new(tls_method_compat());
        if (!ctx_)
            return;

        if (sni_ctx_data_index < 0)
            sni_ctx_data_index =
                SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);

        SSL_CTX_set_ex_data(
            ctx_, sni_ctx_data_index, const_cast<tls_context_data*>(&cd));

        if (cd.servername_callback)
            SSL_CTX_set_tlsext_servername_callback(ctx_, sni_callback);

        // ALPN server-side selection. The callback only fires when this
        // context is used as a server; the client offer (encoded once here)
        // is set per-SSL from alpn_wire_. Snapshot the preference list so the
        // callback and the client offer share one immutable source.
        if (!cd.alpn_protocols.empty())
        {
            alpn_snapshot_ = cd.alpn_protocols;
            SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, &alpn_snapshot_);
            alpn_wire_ = build_alpn_wire(cd.alpn_protocols);
        }

        SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#if defined(SSL_MODE_RELEASE_BUFFERS)
        SSL_CTX_set_mode(ctx_, SSL_MODE_RELEASE_BUFFERS);
#endif

        // Enforce the configured protocol version window (role-agnostic).
        // An inverted window (min > max) admits no protocol; fail closed
        // rather than silently negotiate an unexpected version.
        if (cd.min_version > cd.max_version)
            setup_failed_ = true;
        if (!SSL_CTX_set_min_proto_version(
                ctx_, openssl_proto_version(cd.min_version)))
            setup_failed_ = true;
        if (!SSL_CTX_set_max_proto_version(
                ctx_, openssl_proto_version(cd.max_version)))
            setup_failed_ = true;

        int verify_mode_flag = SSL_VERIFY_NONE;
        if (cd.verification_mode == tls_verify_mode::peer)
            verify_mode_flag = SSL_VERIFY_PEER;
        else if (cd.verification_mode == tls_verify_mode::require_peer)
            verify_mode_flag =
                SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        // The trampoline runs the revocation soft-fail downgrade and the
        // user callback, so install it if either is configured.
        bool const need_trampoline =
            cd.verify_callback ||
            cd.revocation != tls_revocation_policy::disabled;
        SSL_CTX_set_verify(
            ctx_, verify_mode_flag,
            need_trampoline ? &verify_callback_trampoline : nullptr);

        // PKCS#12 bundle: decode cert + key + chain directly into the
        // context. This is an alternative credential source; the PEM/DER
        // fields below are only consulted when no bundle is supplied.
        if (!cd.pkcs12_data.empty())
        {
            // A bundle that fails to decode or parse (wrong passphrase,
            // malformed) must not leave the context silently credential-less:
            // a client using PKCS#12 for mTLS would then fail open against a
            // verify_mode::peer server. Fail closed like every other setup
            // error.
            BIO* bio = BIO_new_mem_buf(
                cd.pkcs12_data.data(),
                static_cast<int>(cd.pkcs12_data.size()));
            if (!bio)
                setup_failed_ = true;
            else
            {
                PKCS12* p12 = d2i_PKCS12_bio(bio, nullptr);
                if (!p12)
                    setup_failed_ = true;
                else
                {
                    EVP_PKEY* pkey        = nullptr;
                    X509* cert            = nullptr;
                    STACK_OF(X509)* chain = nullptr;
                    if (PKCS12_parse(
                            p12, cd.pkcs12_password.c_str(), &pkey, &cert,
                            &chain))
                    {
                        if (cert)
                            SSL_CTX_use_certificate(ctx_, cert);
                        if (pkey)
                            SSL_CTX_use_PrivateKey(ctx_, pkey);
                        if (chain)
                            for (int i = 0; i < sk_X509_num(chain); ++i)
                            {
                                // add_extra_chain_cert takes ownership of the
                                // dup only on success; free it (and fail
                                // closed) otherwise so a partial chain isn't
                                // sent silently.
                                X509* dup = X509_dup(sk_X509_value(chain, i));
                                if (!dup ||
                                    !SSL_CTX_add_extra_chain_cert(ctx_, dup))
                                {
                                    X509_free(dup);
                                    setup_failed_ = true;
                                }
                            }
                    }
                    else
                        setup_failed_ = true;
                    EVP_PKEY_free(pkey);
                    X509_free(cert);
                    if (chain)
                        sk_X509_pop_free(chain, X509_free);
                    PKCS12_free(p12);
                }
                ERR_clear_error();
                BIO_free(bio);
            }
        }

        if (cd.pkcs12_data.empty() && !cd.entity_certificate.empty())
        {
            // An entity certificate that fails to parse must not pass
            // silently: the handshake would run without the identity the
            // caller configured and fail remotely instead of at setup.
            BIO* bio = BIO_new_mem_buf(
                cd.entity_certificate.data(),
                static_cast<int>(cd.entity_certificate.size()));
            if (bio)
            {
                X509* cert = nullptr;
                if (cd.entity_cert_format == tls_file_format::pem)
                    cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
                else
                    cert = d2i_X509_bio(bio, nullptr);
                if (cert)
                {
                    SSL_CTX_use_certificate(ctx_, cert);
                    X509_free(cert);
                }
                else
                {
                    setup_failed_ = true;
                }
                BIO_free(bio);
            }
            else
            {
                setup_failed_ = true;
            }
        }

        if (cd.pkcs12_data.empty() && !cd.certificate_chain.empty())
        {
            BIO* bio = BIO_new_mem_buf(
                cd.certificate_chain.data(),
                static_cast<int>(cd.certificate_chain.size()));
            if (bio)
            {
                X509* entity =
                    PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
                if (entity)
                {
                    SSL_CTX_use_certificate(ctx_, entity);
                    X509_free(entity);
                }

                X509* cert;
                while ((cert = PEM_read_bio_X509(
                            bio, nullptr, nullptr, nullptr)) != nullptr)
                {
                    SSL_CTX_add_extra_chain_cert(ctx_, cert);
                }
                ERR_clear_error();
                BIO_free(bio);
            }
        }

        if (cd.pkcs12_data.empty() && !cd.private_key.empty())
        {
            BIO* bio = BIO_new_mem_buf(
                cd.private_key.data(), static_cast<int>(cd.private_key.size()));
            if (bio)
            {
                EVP_PKEY* pkey = nullptr;
                if (cd.private_key_format == tls_file_format::pem)
                {
                    if (cd.password_callback)
                        pkey = PEM_read_bio_PrivateKey(
                            bio, nullptr, password_callback,
                            const_cast<tls_context_data*>(&cd));
                    else
                        pkey = PEM_read_bio_PrivateKey(
                            bio, nullptr, nullptr, nullptr);
                }
                else
                    pkey = d2i_PrivateKey_bio(bio, nullptr);
                // A key that fails to parse or decrypt (wrong or missing
                // password) must fail setup, not surface later as an
                // inexplicable handshake error.
                if (pkey)
                {
                    SSL_CTX_use_PrivateKey(ctx_, pkey);
                    EVP_PKEY_free(pkey);
                }
                else
                {
                    setup_failed_ = true;
                }
                BIO_free(bio);
                ERR_clear_error();
            }
            else
            {
                setup_failed_ = true;
            }
        }

        X509_STORE* store = SSL_CTX_get_cert_store(ctx_);
        for (auto const& ca : cd.ca_certificates)
        {
            // A trust anchor that fails to parse or add must not pass
            // silently: the store would verify against fewer anchors than
            // requested and reject a legitimate peer as untrusted. Fail
            // closed instead, tolerating only a duplicate the store already
            // holds.
            BIO* bio = BIO_new_mem_buf(ca.data(), static_cast<int>(ca.size()));
            if (!bio)
            {
                setup_failed_ = true;
                continue;
            }
            X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            if (cert)
            {
                if (X509_STORE_add_cert(store, cert) != 1 &&
                    ERR_GET_REASON(ERR_peek_last_error()) !=
                        X509_R_CERT_ALREADY_IN_HASH_TABLE)
                    setup_failed_ = true;
                X509_free(cert);
            }
            else
                setup_failed_ = true;
            ERR_clear_error();
            BIO_free(bio);
        }

        // Trust anchors from the system store and explicit directories.
        // Failures leave the affected source unloaded rather than aborting
        // context creation; the error queue is cleared so it does not leak
        // into a later handshake.
        if (cd.use_default_verify_paths)
            SSL_CTX_set_default_verify_paths(ctx_);
        for (auto const& path : cd.verify_paths)
            SSL_CTX_load_verify_locations(ctx_, nullptr, path.c_str());
        ERR_clear_error();

        // Certificate revocation via CRLs. Load any supplied CRLs and, when
        // a revocation policy is active, enable leaf CRL checking. soft_fail
        // vs hard_fail is applied in the verify trampoline. CRL_CHECK (leaf
        // only) is used so a missing CRL for a trusted root is not itself an
        // error.
        if (cd.revocation != tls_revocation_policy::disabled)
        {
            for (auto const& crl_data : cd.crls)
            {
                BIO* bio = BIO_new_mem_buf(
                    crl_data.data(), static_cast<int>(crl_data.size()));
                if (!bio)
                {
                    setup_failed_ = true;
                    continue;
                }
                // Accept PEM or DER (the documented contract). Try PEM first,
                // then rewind and try DER.
                X509_CRL* crl =
                    PEM_read_bio_X509_CRL(bio, nullptr, nullptr, nullptr);
                if (!crl)
                {
                    BIO_reset(bio);
                    crl = d2i_X509_CRL_bio(bio, nullptr);
                }
                if (crl)
                {
                    X509_STORE_add_crl(store, crl);
                    X509_CRL_free(crl);
                }
                else
                {
                    // A supplied CRL that parses as neither PEM nor DER must
                    // not be silently dropped; record it so the handshake
                    // fails closed rather than weakening revocation.
                    setup_failed_ = true;
                }
                BIO_free(bio);
            }
            X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
            ERR_clear_error();
        }

        SSL_CTX_set_verify_depth(ctx_, cd.verify_depth);

        // Cipher configuration. TLS 1.2-and-below use the cipher list;
        // TLS 1.3 uses the separate ciphersuites API. The security level
        // is deliberately left at the library default: a weak cipher
        // string should fail loudly rather than be silently permitted via
        // a forced @SECLEVEL=0. Callers that genuinely need a lower level
        // can express it in the cipher string (e.g. "...:@SECLEVEL=0").
        // A cipher string the library rejects must not silently fall back to
        // the default suites; fail closed instead.
        if (!cd.ciphersuites.empty() &&
            !SSL_CTX_set_cipher_list(ctx_, cd.ciphersuites.c_str()))
            setup_failed_ = true;
        if (!cd.ciphersuites_tls13.empty() &&
            !SSL_CTX_set_ciphersuites(ctx_, cd.ciphersuites_tls13.c_str()))
            setup_failed_ = true;
    }

    ~openssl_native_context() override
    {
        if (ctx_)
            SSL_CTX_free(ctx_);
    }
};

inline openssl_native_context*
get_openssl_native_context(tls_context_data const& cd)
{
    static char key;
    auto* p = cd.find(&key, [&] { return new openssl_native_context(cd); });
    return static_cast<openssl_native_context*>(p);
}

//
// engine
//

namespace openssl {

engine::~engine()
{
    if (ext_bio_)
        BIO_free(ext_bio_);
    if (ssl_)
        SSL_free(ssl_);
}

std::error_code
engine::init(tls_context const& ctx)
{
    auto& cd = get_tls_context_data(ctx);
    nc_      = get_openssl_native_context(cd);
    if (!nc_->ctx_)
    {
        // The cache retains a failed context build permanently (it
        // never retries), so a later construction can reach here
        // with an already-drained error queue; make_openssl_error(0)
        // is falsy and would let the caller treat this as success and
        // dereference a null ssl_ on first use. Report unconditionally
        // rather than trust ERR_get_error() to be nonzero.
        return std::make_error_code(std::errc::not_enough_memory);
    }

    ssl_ = SSL_new(nc_->ctx_);
    if (!ssl_)
    {
        unsigned long err = ERR_get_error();
        return make_openssl_error(err);
    }

    BIO* int_bio = nullptr;
    if (!BIO_new_bio_pair(&int_bio, 0, &ext_bio_, 0))
    {
        unsigned long err = ERR_get_error();
        SSL_free(ssl_);
        ssl_ = nullptr;
        return make_openssl_error(err);
    }

    SSL_set_bio(ssl_, int_bio, int_bio);

    return {};
}

void
engine::reset()
{
    if (!ssl_)
        return;

    // Preserves SSL* and BIO pair, releases session state
    if (SSL_clear(ssl_) != 1)
        clear_failed_ = true;

    // SSL_clear() retains the negotiated session so a subsequent
    // handshake on this SSL* can resume it. Resumed handshakes skip
    // certificate/hostname re-verification, which would let a changed
    // set_hostname() go unchecked after reset(); drop it to force a
    // full handshake. A failure leaves the old session resumable,
    // defeating that guarantee, so latch the same failure flag
    // SSL_clear() uses above.
    if (SSL_set_session(ssl_, nullptr) != 1)
        clear_failed_ = true;

    // Drain stale data from the external BIO. Mirrors the driver's
    // flush guard: a read failure here would otherwise spin the loop
    // forever since the pending count never advances.
    char drain[1024];
    while (BIO_ctrl_pending(ext_bio_) > 0)
    {
        if (BIO_read(ext_bio_, drain, sizeof(drain)) <= 0)
            break;
    }
}

bool
engine::context_setup_failed() const noexcept
{
    return nc_->setup_failed_;
}

std::error_code
engine::check_context() const noexcept
{
    if (context_setup_failed())
        return std::make_error_code(std::errc::invalid_argument);
    return {};
}

std::error_code
engine::check_session() const noexcept
{
    if (clear_failed_)
        return std::make_error_code(std::errc::invalid_argument);
    return {};
}

std::error_code
engine::prepare(tls_context const&, tls_role role, std::string const& hostname)
{
    // The hostname applies to client handshakes only; a server
    // handshake clears any name left by a prior client-role
    // handshake so client certificates are never hostname-matched.
    std::string const no_name;
    if (!apply_hostname(role == tls_role::client ? hostname : no_name))
    {
        // Fail closed rather than handshake without the requested
        // name check.
        return std::make_error_code(std::errc::invalid_argument);
    }

    // Client offers its ALPN protocol list; the server selects via
    // the context callback. Role is only known here, so install the
    // offer per-session; fail closed rather than negotiate nothing
    // silently.
    if (role == tls_role::client && !apply_alpn_offer())
        return std::make_error_code(std::errc::invalid_argument);

    return {};
}

bool
engine::apply_hostname(std::string const& hostname)
{
    return apply_hostname_verification(ssl_, hostname);
}

bool
engine::apply_alpn_offer()
{
    if (nc_->alpn_wire_.empty())
        return true;

    // SSL_set_alpn_protos uses the inverted convention: 0 = success.
    // A non-zero return (allocation failure) means the offer was not
    // installed; the caller fails closed rather than negotiate nothing
    // silently.
    return SSL_set_alpn_protos(
               ssl_,
               reinterpret_cast<unsigned char const*>(nc_->alpn_wire_.data()),
               static_cast<unsigned int>(nc_->alpn_wire_.size())) == 0;
}

void
engine::capture_alpn(std::string& out) const
{
    unsigned char const* data = nullptr;
    unsigned int len          = 0;
    SSL_get0_alpn_selected(ssl_, &data, &len);
    if (data && len)
        out.assign(reinterpret_cast<char const*>(data), len);
}

engine_result
engine::perform(engine_op op, void* data, std::size_t len)
{
    ERR_clear_error();

    int ret = 0;
    switch (op)
    {
    case engine_op::handshake_client:
        ret = SSL_connect(ssl_);
        break;
    case engine_op::handshake_server:
        ret = SSL_accept(ssl_);
        break;
    case engine_op::read:
        ret = SSL_read(ssl_, data, static_cast<int>(len));
        break;
    case engine_op::write:
        ret = SSL_write(ssl_, data, static_cast<int>(len));
        break;
    case engine_op::shutdown:
        ret = SSL_shutdown(ssl_);
        break;
    }

    bool const transfer = op == engine_op::read || op == engine_op::write;

    // SSL_shutdown returning 0 means our close_notify was queued but
    // the peer's has not arrived yet: flush it, then read for it.
    if (op == engine_op::shutdown && ret == 0)
        return {
            pending_output() > 0 ? engine_want::output_then_retry
                                 : engine_want::input,
            {}, 0};

    if (transfer ? ret > 0 : ret == 1)
        return {
            pending_output() > 0 ? engine_want::output_then_done
                                 : engine_want::done,
            {}, transfer ? static_cast<std::size_t>(ret) : 0};

    int const err = SSL_get_error(ssl_, ret);

    if (err == SSL_ERROR_WANT_WRITE)
        return {engine_want::output_then_retry, {}, 0};

    if (err == SSL_ERROR_WANT_READ)
        return {
            pending_output() > 0 ? engine_want::output_then_retry
                                 : engine_want::input,
            {}, 0};

    if (transfer && err == SSL_ERROR_ZERO_RETURN)
    {
        // ZERO_RETURN means the peer's close_notify WAS received (an
        // announced close), unlike the terminal branch's empty-queue
        // fallback below, which means an unannounced one; report eof,
        // not stream_truncated. A received close_notify queues no
        // output, so a plain done skips the flush.
        return {engine_want::done, make_error_code(capy::error::eof), 0};
    }

    std::error_code ec;
    if (op == engine_op::shutdown)
    {
        unsigned long ssl_err = ERR_get_error();
        if (ssl_err == 0 && err == SSL_ERROR_SYSCALL)
        {
            // The socket closed without an OpenSSL-level error, but
            // that can mean either the peer's close_notify already
            // arrived (this operation's fill, or a concurrent reader,
            // consumed it) or the peer vanished mid-shutdown without
            // ever sending one; only RECEIVED_SHUTDOWN tells them
            // apart, and the documented contract promises
            // stream_truncated for the latter, matching the read path
            // and the driver's `map_fill_error` policy.
            ec = received_shutdown()
                ? std::error_code{}
                : make_error_code(capy::error::stream_truncated);
        }
        else
        {
            ec = make_openssl_error(ssl_err);
        }
    }
    else
    {
        // SYSCALL and every other terminal code map the same way;
        // map_openssl_fatal() covers both.
        ec = map_openssl_fatal();
    }
    return {
        pending_output() > 0 ? engine_want::output_then_done
                             : engine_want::done,
        ec, 0};
}

std::size_t
engine::put_input(unsigned char const* data, std::size_t len)
{
    int put = BIO_write(ext_bio_, data, static_cast<int>(len));
    return put > 0 ? static_cast<std::size_t>(put) : 0;
}

std::pair<unsigned char*, std::size_t>
engine::input_area()
{
    // A BIO pair hands out a pointer into its own buffer, so the
    // transport reads ciphertext directly into the pair with no staging
    // copy. The buffer is circular: this is only the contiguous run to
    // the wrap, which the driver's read loop already tolerates.
    char* p       = nullptr;
    int const cap = BIO_nwrite0(ext_bio_, &p);
    if (cap <= 0 || !p)
        return {nullptr, 0};
    return {reinterpret_cast<unsigned char*>(p), static_cast<std::size_t>(cap)};
}

void
engine::input_committed(std::size_t n)
{
    if (n == 0)
        return;
    // The bytes were written straight into the region BIO_nwrite0
    // returned; advance the pair's write cursor to make them readable.
    char* p = nullptr;
    BIO_nwrite(ext_bio_, &p, static_cast<int>(n));
}

std::size_t
engine::pending_output() const
{
    return BIO_ctrl_pending(ext_bio_);
}

std::size_t
engine::get_output(unsigned char* data, std::size_t len)
{
    int r = BIO_read(ext_bio_, data, static_cast<int>(len));
    return r > 0 ? static_cast<std::size_t>(r) : 0;
}

bool
engine::received_shutdown() const
{
    return (SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN) != 0;
}

} // namespace openssl

} // namespace detail

} // namespace boost::corosio
