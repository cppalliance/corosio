//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/write.hpp>

// Internal context implementation
#include "src/tls/detail/context_impl.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/pkcs12.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

/*
    openssl_stream Architecture
    ===========================

    TLS layer wrapping an underlying stream (via any_stream). Supports one
    concurrent read_some and one concurrent write_some (like Asio's ssl::stream).

    Data Flow (using BIO pairs)
    ---------------------------
    App -> SSL_write -> int_bio_ -> BIO_read(ext_bio_) -> out_buf_ -> s_->write_some -> Network
    App <- SSL_read  <- int_bio_ <- BIO_write(ext_bio_) <- in_buf_ <- s_->read_some  <- Network

    WANT_READ / WANT_WRITE Pattern
    ------------------------------
    OpenSSL's SSL_read/SSL_write return SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE
    when they need I/O. Our coroutine handles this by:

      1. Call SSL_read or SSL_write
      2. Check for pending output in ext_bio_ via BIO_ctrl_pending
      3. If output pending: write to network via s_->write_some
      4. If SSL_ERROR_WANT_READ: read from network into ext_bio_ via s_->read_some + BIO_write
      5. Loop back to step 1

    Renegotiation causes cross-direction I/O: SSL_read may need to write
    handshake data, SSL_write may need to read. Each operation handles
    whatever I/O direction OpenSSL requests.
*/

namespace boost::corosio {

namespace {

constexpr std::size_t default_buffer_size = 16384;

inline SSL_METHOD const*
tls_method_compat() noexcept
{
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    return TLS_method();
#else
    return SSLv23_method();
#endif
}

inline void
apply_hostname_verification(SSL* ssl, std::string const& hostname)
{
    if (hostname.empty())
        return;

    SSL_set_tlsext_host_name(ssl, hostname.c_str());

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_set1_host(ssl, hostname.c_str());
#else
    if (auto* param = SSL_get0_param(ssl))
        X509_VERIFY_PARAM_set1_host(param, hostname.c_str(), 0);
#endif
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

inline std::error_code
normalize_openssl_shutdown_read_error(std::error_code ec) noexcept
{
    if (!ec)
        return ec;

    if (ec == make_error_code(capy::error::eof) ||
        ec == make_error_code(capy::error::canceled) ||
        ec == std::errc::connection_reset ||
        ec == std::errc::connection_aborted || ec == std::errc::broken_pipe)
        return make_error_code(capy::error::stream_truncated);

    return ec;
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
                BIO_free(bio);
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
                if (pkey)
                {
                    SSL_CTX_use_PrivateKey(ctx_, pkey);
                    EVP_PKEY_free(pkey);
                }
                BIO_free(bio);
            }
        }

        X509_STORE* store = SSL_CTX_get_cert_store(ctx_);
        for (auto const& ca : cd.ca_certificates)
        {
            BIO* bio = BIO_new_mem_buf(ca.data(), static_cast<int>(ca.size()));
            if (bio)
            {
                X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
                if (cert)
                {
                    X509_STORE_add_cert(store, cert);
                    X509_free(cert);
                }
                BIO_free(bio);
            }
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

SSL_CTX*
get_openssl_context(tls_context_data const& cd)
{
    return get_openssl_native_context(cd)->ctx_;
}

} // namespace detail

struct openssl_stream::impl
{
    capy::any_stream* s_;
    tls_context ctx_;
    SSL* ssl_     = nullptr;
    BIO* ext_bio_ = nullptr;
    bool used_    = false;

    // ALPN protocol negotiated during the handshake (empty if none).
    std::string alpn_selected_;

    std::vector<char> in_buf_;
    std::vector<char> out_buf_;

    capy::async_mutex io_cm_;

    impl(capy::any_stream& s, tls_context ctx) : s_(&s), ctx_(std::move(ctx))
    {
        in_buf_.resize(default_buffer_size);
        out_buf_.resize(default_buffer_size);
    }

    ~impl()
    {
        if (ext_bio_)
            BIO_free(ext_bio_);
        if (ssl_)
            SSL_free(ssl_);
    }

    void reset()
    {
        if (!ssl_)
            return;

        // Preserves SSL* and BIO pair, releases session state
        SSL_clear(ssl_);

        // Drain stale data from the external BIO
        char drain[1024];
        while (BIO_ctrl_pending(ext_bio_) > 0)
            BIO_read(ext_bio_, drain, sizeof(drain));

        // SSL_clear clears per-session settings; reapply hostname
        auto& cd = detail::get_tls_context_data(ctx_);
        apply_hostname_verification(ssl_, cd.hostname);

        alpn_selected_.clear();
        used_ = false;
    }

    // Record the ALPN protocol selected during the handshake, if any.
    void capture_alpn()
    {
        unsigned char const* data = nullptr;
        unsigned int len          = 0;
        SSL_get0_alpn_selected(ssl_, &data, &len);
        if (data && len)
            alpn_selected_.assign(reinterpret_cast<char const*>(data), len);
    }

    capy::task<std::error_code> flush_output()
    {
        while (BIO_ctrl_pending(ext_bio_) > 0)
        {
            std::size_t got = 0;
            while (BIO_ctrl_pending(ext_bio_) > 0 && got < out_buf_.size())
            {
                int put = static_cast<int>(BIO_ctrl_pending(ext_bio_));
                put = (std::min)(put, static_cast<int>(out_buf_.size() - got));
                int r = BIO_read(ext_bio_, out_buf_.data() + got, put);
                if (r <= 0)
                    break;
                got += static_cast<std::size_t>(r);
            }
            if (got == 0)
                break;

            {
                auto [lec] = co_await io_cm_.lock();
                if (lec)
                    co_return lec;
                capy::async_mutex::lock_guard io_guard(&io_cm_);
                auto [ec, n] = co_await capy::write(
                    *s_, capy::const_buffer(out_buf_.data(), got));
                if (ec)
                    co_return ec;
            }
        }
        co_return std::error_code{};
    }

    capy::task<std::error_code> read_input()
    {
        auto [lec] = co_await io_cm_.lock();
        if (lec)
            co_return lec;
        capy::async_mutex::lock_guard io_guard(&io_cm_);
        auto [ec, n] = co_await s_->read_some(
            capy::mutable_buffer(in_buf_.data(), in_buf_.size()));
        if (ec)
            co_return ec;

        int got = BIO_write(ext_bio_, in_buf_.data(), static_cast<int>(n));
        if (got < static_cast<int>(n))
        {
            co_return make_error_code(std::errc::no_buffer_space);
        }

        co_return std::error_code{};
    }

    capy::io_task<std::size_t>
    do_read_some(capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
    {
        std::error_code ec;
        std::size_t total_read = 0;

        for (auto& buf : buffers)
        {
            char* dest    = static_cast<char*>(buf.data());
            int remaining = static_cast<int>(buf.size());

            while (remaining > 0)
            {
                ERR_clear_error();
                int ret = SSL_read(ssl_, dest, remaining);

                if (ret > 0)
                {
                    dest += ret;
                    remaining -= ret;
                    total_read += static_cast<std::size_t>(ret);

                    if (total_read > 0)
                        co_return {std::error_code{}, total_read};
                }
                else
                {
                    int err = SSL_get_error(ssl_, ret);

                    if (err == SSL_ERROR_WANT_WRITE)
                    {
                        ec = co_await flush_output();
                        if (ec)
                            co_return {ec, total_read};
                    }
                    else if (err == SSL_ERROR_WANT_READ)
                    {
                        ec = co_await flush_output();
                        if (ec)
                            co_return {ec, total_read};

                        ec = co_await read_input();
                        if (ec)
                        {
                            if (ec == make_error_code(capy::error::eof))
                            {
                                if (SSL_get_shutdown(ssl_) &
                                    SSL_RECEIVED_SHUTDOWN)
                                    ec = make_error_code(capy::error::eof);
                                else
                                    ec = make_error_code(
                                        capy::error::stream_truncated);
                            }
                            co_return {ec, total_read};
                        }
                    }
                    else if (err == SSL_ERROR_ZERO_RETURN)
                    {
                        co_return {
                            make_error_code(capy::error::eof), total_read};
                    }
                    else if (err == SSL_ERROR_SYSCALL)
                    {
                        unsigned long ssl_err = ERR_get_error();
                        if (ssl_err == 0)
                            ec = make_error_code(capy::error::stream_truncated);
                        else
                            ec = make_openssl_error(ssl_err);
                        co_return {ec, total_read};
                    }
                    else
                    {
                        unsigned long ssl_err = ERR_get_error();
                        ec                    = make_openssl_error(ssl_err);
                        co_return {ec, total_read};
                    }
                }
            }
        }

        co_return {std::error_code{}, total_read};
    }

    capy::io_task<std::size_t>
    do_write_some(capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
    {
        std::error_code ec;
        std::size_t total_written = 0;

        for (auto const& buf : buffers)
        {
            char const* src = static_cast<char const*>(buf.data());
            int remaining   = static_cast<int>(buf.size());

            while (remaining > 0)
            {
                ERR_clear_error();
                int ret = SSL_write(ssl_, src, remaining);

                if (ret > 0)
                {
                    src += ret;
                    remaining -= ret;
                    total_written += static_cast<std::size_t>(ret);

                    if (total_written > 0)
                    {
                        ec = co_await flush_output();
                        co_return {ec, total_written};
                    }
                }
                else
                {
                    int err = SSL_get_error(ssl_, ret);

                    if (err == SSL_ERROR_WANT_WRITE)
                    {
                        ec = co_await flush_output();
                        if (ec)
                            co_return {ec, total_written};
                    }
                    else if (err == SSL_ERROR_WANT_READ)
                    {
                        ec = co_await flush_output();
                        if (ec)
                            co_return {ec, total_written};

                        ec = co_await read_input();
                        if (ec)
                            co_return {ec, total_written};
                    }
                    else
                    {
                        unsigned long ssl_err = ERR_get_error();
                        ec                    = make_openssl_error(ssl_err);
                        co_return {ec, total_written};
                    }
                }
            }
        }

        co_return {std::error_code{}, total_written};
    }

    capy::io_task<> do_handshake(int type)
    {
        // A requested configuration could not be applied when the native
        // context was built (inverted protocol window, rejected cipher/
        // version, or an unparseable CRL). Refuse the handshake rather than
        // proceed with weakened or unexpected settings.
        auto* nc = detail::get_openssl_native_context(
            detail::get_tls_context_data(ctx_));
        if (nc->setup_failed_)
            co_return std::make_error_code(std::errc::invalid_argument);

        if (used_)
            reset();

        std::error_code ec;

        // Client offers its ALPN protocol list; the server selects via the
        // context callback. Role is only known here, so set the pre-encoded
        // wire offer per-SSL.
        if (type == openssl_stream::client && !nc->alpn_wire_.empty())
        {
            // SSL_set_alpn_protos uses the inverted convention: 0 = success.
            // A non-zero return (allocation failure) means the offer was not
            // installed; fail closed rather than negotiate nothing silently.
            if (SSL_set_alpn_protos(
                    ssl_,
                    reinterpret_cast<unsigned char const*>(
                        nc->alpn_wire_.data()),
                    static_cast<unsigned int>(nc->alpn_wire_.size())) != 0)
                co_return std::make_error_code(std::errc::invalid_argument);
        }

        while (true)
        {
            ERR_clear_error();
            int ret;
            if (type == openssl_stream::client)
                ret = SSL_connect(ssl_);
            else
                ret = SSL_accept(ssl_);

            if (ret == 1)
            {
                used_ = true;
                capture_alpn();
                ec = co_await flush_output();
                co_return {ec};
            }
            else
            {
                int err = SSL_get_error(ssl_, ret);

                if (err == SSL_ERROR_WANT_WRITE)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec};
                }
                else if (err == SSL_ERROR_WANT_READ)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec};

                    ec = co_await read_input();
                    if (ec)
                        co_return {ec};
                }
                else
                {
                    unsigned long ssl_err = ERR_get_error();
                    ec                    = make_openssl_error(ssl_err);
                    co_return {ec};
                }
            }
        }
    }

    capy::io_task<> do_shutdown()
    {
        std::error_code ec;

        while (true)
        {
            ERR_clear_error();
            int ret = SSL_shutdown(ssl_);

            if (ret == 1)
            {
                ec = co_await flush_output();
                co_return {ec};
            }
            else if (ret == 0)
            {
                ec = co_await flush_output();
                if (ec)
                    co_return {ec};

                ec = co_await read_input();
                if (ec)
                {
                    ec = normalize_openssl_shutdown_read_error(ec);
                    co_return {ec};
                }
            }
            else
            {
                int err = SSL_get_error(ssl_, ret);

                if (err == SSL_ERROR_WANT_WRITE)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec};
                }
                else if (err == SSL_ERROR_WANT_READ)
                {
                    ec = co_await flush_output();
                    if (ec)
                        co_return {ec};

                    ec = co_await read_input();
                    if (ec)
                    {
                        ec = normalize_openssl_shutdown_read_error(ec);
                        co_return {ec};
                    }
                }
                else
                {
                    unsigned long ssl_err = ERR_get_error();
                    if (ssl_err == 0 && err == SSL_ERROR_SYSCALL)
                    {
                        ec = {};
                    }
                    else
                    {
                        ec = make_openssl_error(ssl_err);
                    }
                    co_return {ec};
                }
            }
        }
    }

    std::error_code init_ssl()
    {
        auto& cd            = detail::get_tls_context_data(ctx_);
        SSL_CTX* native_ctx = detail::get_openssl_context(cd);
        if (!native_ctx)
        {
            unsigned long err = ERR_get_error();
            return make_openssl_error(err);
        }

        ssl_ = SSL_new(native_ctx);
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

        apply_hostname_verification(ssl_, cd.hostname);

        return {};
    }
};

openssl_stream::impl*
openssl_stream::make_impl(capy::any_stream& stream, tls_context const& ctx)
{
    auto* p = new impl(stream, ctx);

    auto ec = p->init_ssl();
    if (ec)
    {
        delete p;
        return nullptr;
    }

    return p;
}

openssl_stream::~openssl_stream()
{
    delete impl_;
}

openssl_stream::openssl_stream(openssl_stream&& other) noexcept
    : stream_(std::move(other.stream_))
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
    if (impl_)
        impl_->s_ = &stream_;
}

openssl_stream&
openssl_stream::operator=(openssl_stream&& other) noexcept
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
openssl_stream::do_read_some(
    capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_read_some(buffers);
}

capy::io_task<std::size_t>
openssl_stream::do_write_some(
    capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
{
    co_return co_await impl_->do_write_some(buffers);
}

capy::io_task<>
openssl_stream::handshake(handshake_type type)
{
    co_return co_await impl_->do_handshake(type);
}

capy::io_task<>
openssl_stream::shutdown()
{
    co_return co_await impl_->do_shutdown();
}

void
openssl_stream::reset()
{
    impl_->reset();
}

std::string_view
openssl_stream::name() const noexcept
{
    return "openssl";
}

std::string_view
openssl_stream::alpn_protocol() const noexcept
{
    return impl_->alpn_selected_;
}

} // namespace boost::corosio
