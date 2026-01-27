//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tls/openssl_stream.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/task.hpp>

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

    TLS layer wrapping an underlying io_stream. Supports one concurrent
    read_some and one concurrent write_some (like Asio's ssl::stream).

    Data Flow (using BIO pairs)
    ---------------------------
    App -> SSL_write -> int_bio_ -> BIO_read(ext_bio_) -> out_buf_ -> s_.write_some -> Network
    App <- SSL_read  <- int_bio_ <- BIO_write(ext_bio_) <- in_buf_ <- s_.read_some  <- Network

    WANT_READ / WANT_WRITE Pattern
    ------------------------------
    OpenSSL's SSL_read/SSL_write return SSL_ERROR_WANT_READ or SSL_ERROR_WANT_WRITE
    when they need I/O. Our coroutine handles this by:

      1. Call SSL_read or SSL_write
      2. Check for pending output in ext_bio_ via BIO_ctrl_pending
      3. If output pending: write to network via s_.write_some
      4. If SSL_ERROR_WANT_READ: read from network into ext_bio_ via s_.read_some + BIO_write
      5. Loop back to step 1

    Renegotiation causes cross-direction I/O: SSL_read may need to write
    handshake data, SSL_write may need to read. Each operation handles
    whatever I/O direction OpenSSL requests.

    Key Types
    ---------
    - openssl_stream_impl_ : tls_stream_impl  -- the impl stored in io_object::impl_
    - do_read_some, do_write_some             -- inner coroutines with WANT_* loops
*/

namespace boost::corosio {

namespace {

// Default buffer size for TLS I/O
constexpr std::size_t default_buffer_size = 16384;

// Maximum number of buffers to handle in a single operation
constexpr std::size_t max_buffers = 8;

// Buffer array type for coroutine parameters (copied into frame)
using buffer_array = std::array<capy::mutable_buffer, max_buffers>;

} // namespace

//------------------------------------------------------------------------------
//
// Native context caching
//
//------------------------------------------------------------------------------

namespace tls::detail {

// Ex data index for storing context_data pointer in SSL_CTX
static int sni_ctx_data_index = -1;

// SNI callback invoked by OpenSSL during handshake
static int
sni_callback( SSL* ssl, int* /* alert */, void* /* arg */ )
{
    char const* servername = SSL_get_servername( ssl, TLSEXT_NAMETYPE_host_name );
    if( !servername )
        return SSL_TLSEXT_ERR_NOACK;  // No SNI sent, continue

    SSL_CTX* ctx = SSL_get_SSL_CTX( ssl );
    auto* cd = static_cast<context_data const*>(
        SSL_CTX_get_ex_data( ctx, sni_ctx_data_index ) );

    if( cd && cd->servername_callback )
    {
        if( !cd->servername_callback( servername ) )
            return SSL_TLSEXT_ERR_ALERT_FATAL;  // Callback rejected hostname
    }

    return SSL_TLSEXT_ERR_OK;
}

/** Cached OpenSSL context owning SSL_CTX.

    Created on first stream construction for a given tls::context,
    then reused for subsequent streams sharing that context.
*/
class openssl_native_context
    : public native_context_base
{
public:
    SSL_CTX* ctx_;
    context_data const* cd_;  // For SNI callback access

    explicit
    openssl_native_context( context_data const& cd )
        : ctx_( nullptr )
        , cd_( &cd )
    {
        // Create SSL_CTX supporting both client and server
        ctx_ = SSL_CTX_new( TLS_method() );
        if( !ctx_ )
            return;

        // Initialize ex_data index for SNI callback (once)
        if( sni_ctx_data_index < 0 )
            sni_ctx_data_index = SSL_CTX_get_ex_new_index( 0, nullptr, nullptr, nullptr, nullptr );

        // Store context_data pointer for SNI callback access
        SSL_CTX_set_ex_data( ctx_, sni_ctx_data_index, const_cast<context_data*>( &cd ) );

        // Set SNI callback if provided
        if( cd.servername_callback )
            SSL_CTX_set_tlsext_servername_callback( ctx_, sni_callback );

        // Set modes for partial writes and moving buffers
        SSL_CTX_set_mode( ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE );
        SSL_CTX_set_mode( ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER );
#if defined( SSL_MODE_RELEASE_BUFFERS )
        SSL_CTX_set_mode( ctx_, SSL_MODE_RELEASE_BUFFERS );
#endif

        // Apply verify mode from config
        int verify_mode_flag = SSL_VERIFY_NONE;
        if( cd.verification_mode == verify_mode::peer )
            verify_mode_flag = SSL_VERIFY_PEER;
        else if( cd.verification_mode == verify_mode::require_peer )
            verify_mode_flag = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_CTX_set_verify( ctx_, verify_mode_flag, nullptr );

        // Apply certificates if provided
        if( !cd.entity_certificate.empty() )
        {
            BIO* bio = BIO_new_mem_buf(
                cd.entity_certificate.data(),
                static_cast<int>( cd.entity_certificate.size() ) );
            if( bio )
            {
                X509* cert = nullptr;
                if( cd.entity_cert_format == file_format::pem )
                    cert = PEM_read_bio_X509( bio, nullptr, nullptr, nullptr );
                else
                    cert = d2i_X509_bio( bio, nullptr );
                if( cert )
                {
                    SSL_CTX_use_certificate( ctx_, cert );
                    X509_free( cert );
                }
                BIO_free( bio );
            }
        }

        // Apply certificate chain if provided (entity cert + intermediates)
        // First cert is entity, rest are intermediates
        if( !cd.certificate_chain.empty() )
        {
            BIO* bio = BIO_new_mem_buf(
                cd.certificate_chain.data(),
                static_cast<int>( cd.certificate_chain.size() ) );
            if( bio )
            {
                // First cert is entity certificate
                X509* entity = PEM_read_bio_X509( bio, nullptr, nullptr, nullptr );
                if( entity )
                {
                    SSL_CTX_use_certificate( ctx_, entity );
                    X509_free( entity );
                }
                
                // Remaining certs are intermediates - add as extra chain certs
                X509* cert;
                while( ( cert = PEM_read_bio_X509( bio, nullptr, nullptr, nullptr ) ) != nullptr )
                {
                    // SSL_CTX_add_extra_chain_cert takes ownership - don't free
                    SSL_CTX_add_extra_chain_cert( ctx_, cert );
                }
                ERR_clear_error(); // Clear expected EOF error from reading end of chain
                BIO_free( bio );
            }
        }

        // Apply private key if provided
        if( !cd.private_key.empty() )
        {
            BIO* bio = BIO_new_mem_buf(
                cd.private_key.data(),
                static_cast<int>( cd.private_key.size() ) );
            if( bio )
            {
                EVP_PKEY* pkey = nullptr;
                if( cd.private_key_format == file_format::pem )
                    pkey = PEM_read_bio_PrivateKey( bio, nullptr, nullptr, nullptr );
                else
                    pkey = d2i_PrivateKey_bio( bio, nullptr );
                if( pkey )
                {
                    SSL_CTX_use_PrivateKey( ctx_, pkey );
                    EVP_PKEY_free( pkey );
                }
                BIO_free( bio );
            }
        }

        // Apply CA certificates for verification
        X509_STORE* store = SSL_CTX_get_cert_store( ctx_ );
        for( auto const& ca : cd.ca_certificates )
        {
            BIO* bio = BIO_new_mem_buf( ca.data(), static_cast<int>( ca.size() ) );
            if( bio )
            {
                X509* cert = PEM_read_bio_X509( bio, nullptr, nullptr, nullptr );
                if( cert )
                {
                    X509_STORE_add_cert( store, cert );
                    X509_free( cert );
                }
                BIO_free( bio );
            }
        }

        // Apply verify depth
        SSL_CTX_set_verify_depth( ctx_, cd.verify_depth );

        // Apply cipher suites if provided
        if( !cd.ciphersuites.empty() )
        {
            SSL_CTX_set_security_level( ctx_, 0 );
            SSL_CTX_set_cipher_list( ctx_, cd.ciphersuites.c_str() );
        }
    }

    ~openssl_native_context() override
    {
        if( ctx_ )
            SSL_CTX_free( ctx_ );
    }
};

/** Get or create cached SSL_CTX for this context.

    @param cd The context implementation.

    @return Pointer to the cached SSL_CTX.
*/
inline SSL_CTX*
get_openssl_context( context_data const& cd )
{
    static char key;
    auto* p = cd.find( &key, [&]
    {
        return new openssl_native_context( cd );
    });
    return static_cast<openssl_native_context*>( p )->ctx_;
}

} // namespace tls::detail

//------------------------------------------------------------------------------

struct openssl_stream_impl_
    : tls_stream::tls_stream_impl
{
    io_stream& s_;
    tls::context ctx_;      // holds ref to cached native context
    SSL* ssl_ = nullptr;
    BIO* ext_bio_ = nullptr;

    // Buffers for network I/O
    std::vector<char> in_buf_;
    std::vector<char> out_buf_;

    // Renegotiation can cause both TLS read/write to access the socket
    capy::async_mutex io_mutex_;

    //--------------------------------------------------------------------------

    openssl_stream_impl_( io_stream& s, tls::context ctx )
        : s_( s )
        , ctx_( std::move( ctx ) )
    {
        in_buf_.resize( default_buffer_size );
        out_buf_.resize( default_buffer_size );
    }

    ~openssl_stream_impl_()
    {
        if( ext_bio_ )
            BIO_free( ext_bio_ );
        if( ssl_ )
            SSL_free( ssl_ );
        // SSL_CTX* is owned by cached native context, not freed here
    }

    //--------------------------------------------------------------------------
    // Helper to flush pending output from BIO to network
    //--------------------------------------------------------------------------

    capy::task<system::error_code>
    flush_output(std::stop_token token)
    {
        while(BIO_ctrl_pending(ext_bio_) > 0 && !token.stop_requested())
        {
            int pending = static_cast<int>(BIO_ctrl_pending(ext_bio_));
            int to_read = (std::min)(pending, static_cast<int>(out_buf_.size()));
            int n = BIO_read(ext_bio_, out_buf_.data(), to_read);
            if(n <= 0)
                break;

            // Write to underlying stream
            auto guard = co_await io_mutex_.scoped_lock();
            auto [ec, written] = co_await s_.write_some(
                capy::mutable_buffer(out_buf_.data(), static_cast<std::size_t>(n)));
            if(ec)
                co_return ec;
        }
        if(token.stop_requested())
        {
            co_return make_error_code(system::errc::operation_canceled);
        }
        co_return system::error_code{};
    }

    capy::task<system::error_code>
    read_input(std::stop_token token)
    {
        if(token.stop_requested())
        {
            co_return make_error_code(system::errc::operation_canceled);
        }
        auto guard = co_await io_mutex_.scoped_lock();
        auto [ec, n] = co_await s_.read_some(
            capy::mutable_buffer(in_buf_.data(), in_buf_.size()));
        if(ec)
            co_return ec;

        // Feed data into OpenSSL
        int written = BIO_write(ext_bio_, in_buf_.data(), static_cast<int>(n));
        (void)written;

        co_return system::error_code{};
    }

    //--------------------------------------------------------------------------
    // Inner coroutines for TLS read/write operations
    //--------------------------------------------------------------------------

    capy::task<>
    do_read_some(
        buffer_array dest_bufs,
        std::size_t buf_count,
        std::stop_token token,
        system::error_code* ec_out,
        std::size_t* bytes_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;
        std::size_t total_read = 0;

        // Process each destination buffer
        for(std::size_t i = 0; i < buf_count && !token.stop_requested(); ++i)
        {
            char* dest = static_cast<char*>(dest_bufs[i].data());
            int remaining = static_cast<int>(dest_bufs[i].size());

            while(remaining > 0 && !token.stop_requested())
            {
                ERR_clear_error();
                int ret = SSL_read(ssl_, dest, remaining);

                if(ret > 0)
                {
                    dest += ret;
                    remaining -= ret;
                    total_read += static_cast<std::size_t>(ret);

                    // For read_some semantics, return after first successful read
                    if(total_read > 0)
                        goto done;
                }
                else
                {
                    int err = SSL_get_error(ssl_, ret);

                    if(err == SSL_ERROR_WANT_WRITE)
                    {
                        // Flush pending output (renegotiation)
                        ec = co_await flush_output(token);
                        if(ec)
                            goto done;
                    }
                    else if(err == SSL_ERROR_WANT_READ)
                    {
                        // First flush any pending output
                        ec = co_await flush_output(token);
                        if(ec)
                            goto done;

                        // Then read from network
                        ec = co_await read_input(token);
                        if(ec)
                        {
                            if(ec == make_error_code(capy::error::eof))
                            {
                                // Check if we got a proper TLS shutdown
                                if(SSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN)
                                    ec = make_error_code(capy::error::eof);
                                else
                                    ec = make_error_code(capy::error::stream_truncated);
                            }
                            goto done;
                        }
                    }
                    else if(err == SSL_ERROR_ZERO_RETURN)
                    {
                        ec = make_error_code(capy::error::eof);
                        goto done;
                    }
                    else if(err == SSL_ERROR_SYSCALL)
                    {
                        unsigned long ssl_err = ERR_get_error();
                        if(ssl_err == 0)
                            ec = make_error_code(capy::error::stream_truncated);
                        else
                            ec = system::error_code(
                                static_cast<int>(ssl_err), system::system_category());
                        goto done;
                    }
                    else
                    {
                        unsigned long ssl_err = ERR_get_error();
                        ec = system::error_code(
                            static_cast<int>(ssl_err), system::system_category());
                        goto done;
                    }
                }
            }
        }

    done:
        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;
        *bytes_out = total_read;

        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    capy::task<>
    do_write_some(
        buffer_array src_bufs,
        std::size_t buf_count,
        std::stop_token token,
        system::error_code* ec_out,
        std::size_t* bytes_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;
        std::size_t total_written = 0;

        // Process each source buffer
        for(std::size_t i = 0; i < buf_count && !token.stop_requested(); ++i)
        {
            char const* src = static_cast<char const*>(src_bufs[i].data());
            int remaining = static_cast<int>(src_bufs[i].size());

            while(remaining > 0 && !token.stop_requested())
            {
                ERR_clear_error();
                int ret = SSL_write(ssl_, src, remaining);

                if(ret > 0)
                {
                    src += ret;
                    remaining -= ret;
                    total_written += static_cast<std::size_t>(ret);

                    // For write_some semantics, flush and return after first successful write
                    if(total_written > 0)
                    {
                        ec = co_await flush_output(token);
                        goto done;
                    }
                }
                else
                {
                    int err = SSL_get_error(ssl_, ret);

                    if(err == SSL_ERROR_WANT_WRITE)
                    {
                        ec = co_await flush_output(token);
                        if(ec)
                            goto done;
                    }
                    else if(err == SSL_ERROR_WANT_READ)
                    {
                        // Renegotiation - flush then read
                        ec = co_await flush_output(token);
                        if(ec)
                            goto done;

                        ec = co_await read_input(token);
                        if(ec)
                            goto done;
                    }
                    else
                    {
                        unsigned long ssl_err = ERR_get_error();
                        ec = system::error_code(
                            static_cast<int>(ssl_err), system::system_category());
                        goto done;
                    }
                }
            }
        }

    done:
        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;
        *bytes_out = total_written;

        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    capy::task<>
    do_handshake(
        int type,
        std::stop_token token,
        system::error_code* ec_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;

        while(!token.stop_requested())
        {
            ERR_clear_error();
            int ret;
            if(type == openssl_stream::client)
                ret = SSL_connect(ssl_);
            else
                ret = SSL_accept(ssl_);

            if(ret == 1)
            {
                // Handshake completed - flush any remaining output
                ec = co_await flush_output(token);
                break;
            }
            else
            {
                int err = SSL_get_error(ssl_, ret);

                if(err == SSL_ERROR_WANT_WRITE)
                {
                    ec = co_await flush_output(token);
                    if(ec)
                        break;
                }
                else if(err == SSL_ERROR_WANT_READ)
                {
                    // Flush output first (e.g., ClientHello)
                    ec = co_await flush_output(token);
                    if(ec)
                        break;
                    // Then read response
                    ec = co_await read_input(token);
                    if(ec)
                        break;
                }
                else
                {
                    unsigned long ssl_err = ERR_get_error();
                    ec = system::error_code(
                        static_cast<int>(ssl_err), system::system_category());
                    break;
                }
            }
        }

        if(token.stop_requested())
        {
            ec = make_error_code(system::errc::operation_canceled);
        }
        *ec_out = ec;

        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    capy::task<>
    do_shutdown(
        std::stop_token token,
        system::error_code* ec_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;

        while(!token.stop_requested())
        {
            ERR_clear_error();
            int ret = SSL_shutdown(ssl_);

            if(ret == 1)
            {
                // Bidirectional shutdown complete
                ec = co_await flush_output(token);
                break;
            }
            else if(ret == 0)
            {
                // Sent close_notify, need to receive peer's
                ec = co_await flush_output(token);
                if(ec)
                    break;

                // Continue to receive peer's close_notify
                ec = co_await read_input(token);
                if(ec)
                {
                    // EOF is expected during shutdown
                    if(ec == make_error_code(capy::error::eof))
                        ec = {};
                    break;
                }
            }
            else
            {
                int err = SSL_get_error(ssl_, ret);

                if(err == SSL_ERROR_WANT_WRITE)
                {
                    ec = co_await flush_output(token);
                    if(ec)
                        break;
                }
                else if(err == SSL_ERROR_WANT_READ)
                {
                    ec = co_await flush_output(token);
                    if(ec)
                        break;

                    ec = co_await read_input(token);
                    if(ec)
                    {
                        if(ec == make_error_code(capy::error::eof))
                            ec = {};
                        break;
                    }
                }
                else
                {
                    unsigned long ssl_err = ERR_get_error();
                    if(ssl_err == 0 && err == SSL_ERROR_SYSCALL)
                    {
                        // Connection closed without close_notify - acceptable
                        ec = {};
                    }
                    else
                    {
                        ec = system::error_code(
                            static_cast<int>(ssl_err), system::system_category());
                    }
                    break;
                }
            }
        }

        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;

        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    //--------------------------------------------------------------------------
    // io_stream_impl interface
    //--------------------------------------------------------------------------

    void release() override
    {
        delete this;
    }

    void read_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        capy::run_async(d, token)(
            do_read_some(bufs, count, token, ec, bytes, h, d));
    }

    void write_some(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        io_buffer_param param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        capy::run_async(d, token)(
            do_write_some(bufs, count, token, ec, bytes, h, d));
    }

    void handshake(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        int type,
        std::stop_token token,
        system::error_code* ec) override
    {
        capy::run_async(d, token)(
            do_handshake(type, token, ec, h, d));
    }

    void shutdown(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        std::stop_token token,
        system::error_code* ec) override
    {
        capy::run_async(d, token)(
            do_shutdown(token, ec, h, d));
    }

    //--------------------------------------------------------------------------
    // Initialization
    //--------------------------------------------------------------------------

    system::error_code
    init_ssl()
    {
        // Get cached SSL_CTX from tls::context
        auto& impl = tls::detail::get_context_data( ctx_ );
        SSL_CTX* native_ctx = tls::detail::get_openssl_context( impl );
        if( !native_ctx )
        {
            unsigned long err = ERR_get_error();
            return system::error_code(
                static_cast<int>( err ), system::system_category() );
        }

        // Create SSL session from cached context
        ssl_ = SSL_new( native_ctx );
        if( !ssl_ )
        {
            unsigned long err = ERR_get_error();
            return system::error_code(
                static_cast<int>( err ), system::system_category() );
        }

        // Create BIO pair for I/O
        BIO* int_bio = nullptr;
        if( !BIO_new_bio_pair( &int_bio, 0, &ext_bio_, 0 ) )
        {
            unsigned long err = ERR_get_error();
            SSL_free( ssl_ );
            ssl_ = nullptr;
            return system::error_code(
                static_cast<int>( err ), system::system_category() );
        }

        // Attach internal BIO to SSL (SSL takes ownership)
        SSL_set_bio( ssl_, int_bio, int_bio );

        // Apply per-session config (SNI + hostname verification) from context
        if( !impl.hostname.empty() )
        {
            // Set SNI extension so server knows which cert to present
            SSL_set_tlsext_host_name( ssl_, impl.hostname.c_str() );
            
            // Enable hostname verification (checks CN/SAN in peer cert)
            // Available in OpenSSL 1.0.2+
            SSL_set1_host( ssl_, impl.hostname.c_str() );
        }

        return {};
    }
};

//------------------------------------------------------------------------------

openssl_stream::
openssl_stream( io_stream& stream, tls::context ctx )
    : tls_stream( stream )
{
    auto* impl = new openssl_stream_impl_( s_, std::move( ctx ) );

    auto ec = impl->init_ssl();
    if( ec )
    {
        delete impl;
        return;
    }

    impl_ = impl;
}

openssl_stream::
~openssl_stream()
{
    if( impl_ )
        impl_->release();
}

} // namespace boost::corosio
