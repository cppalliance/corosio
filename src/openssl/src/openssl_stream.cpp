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

        SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#if defined(SSL_MODE_RELEASE_BUFFERS)
        SSL_CTX_set_mode(ctx_, SSL_MODE_RELEASE_BUFFERS);
#endif

        int verify_mode_flag = SSL_VERIFY_NONE;
        if (cd.verification_mode == tls_verify_mode::peer)
            verify_mode_flag = SSL_VERIFY_PEER;
        else if (cd.verification_mode == tls_verify_mode::require_peer)
            verify_mode_flag =
                SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_CTX_set_verify(ctx_, verify_mode_flag, nullptr);

        if (!cd.entity_certificate.empty())
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

        if (!cd.certificate_chain.empty())
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

        if (!cd.private_key.empty())
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

        SSL_CTX_set_verify_depth(ctx_, cd.verify_depth);

        if (!cd.ciphersuites.empty())
        {
#ifndef OPENSSL_IS_BORINGSSL
            SSL_CTX_set_security_level(ctx_, 0);
#endif
            SSL_CTX_set_cipher_list(ctx_, cd.ciphersuites.c_str());
        }
    }

    ~openssl_native_context() override
    {
        if (ctx_)
            SSL_CTX_free(ctx_);
    }
};

inline SSL_CTX*
get_openssl_context(tls_context_data const& cd)
{
    static char key;
    auto* p = cd.find(&key, [&] { return new openssl_native_context(cd); });
    return static_cast<openssl_native_context*>(p)->ctx_;
}

} // namespace detail

struct openssl_stream::impl
{
    capy::any_stream* s_;
    tls_context ctx_;
    SSL* ssl_     = nullptr;
    BIO* ext_bio_ = nullptr;
    bool used_    = false;

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

        used_ = false;
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
        if (used_)
            reset();

        std::error_code ec;

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
                ec    = co_await flush_output();
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

} // namespace boost::corosio
