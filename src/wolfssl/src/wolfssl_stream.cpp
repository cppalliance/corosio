//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers/some_buffers.hpp>
#include <boost/capy/ex/coro_lock.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/write.hpp>

// Internal context implementation
#include "src/tls/detail/context_impl.hpp"

// Include WolfSSL options first to get proper feature detection
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

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
    App -> wolfSSL_write -> send_callback -> out_buf_ -> s_.write_some -> Network
    App <- wolfSSL_read  <- recv_callback <- in_buf_  <- s_.read_some  <- Network

    WANT_READ / WANT_WRITE Pattern
    ------------------------------
    WolfSSL's I/O callbacks are synchronous but our underlying stream is async.
    When WolfSSL needs I/O:

      1. Callback checks internal buffer (in_buf_ or out_buf_)
      2. If data available: return it immediately
      3. If not: return WOLFSSL_CBIO_ERR_WANT_READ or WANT_WRITE
      4. wolfSSL_read/write returns WOLFSSL_ERROR_WANT_*
      5. Our coroutine does async I/O: co_await s_.read_some() or write_some()
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

} // namespace

//------------------------------------------------------------------------------
//
// Native context caching
//
//------------------------------------------------------------------------------

namespace detail {

// Password callback invoked by WolfSSL when loading encrypted private keys
static int
wolfssl_password_callback(char* buf, int size, int rwflag, void* userdata)
{
    auto* cd = static_cast<tls_context_data const*>(userdata);
    if(!cd || !cd->password_callback)
        return 0;

    tls_password_purpose purpose = (rwflag == 0)
        ? tls_password_purpose::for_reading
        : tls_password_purpose::for_writing;

    std::string password = cd->password_callback(
        static_cast<std::size_t>(size), purpose);

    int len = static_cast<int>(password.size());
    if(len > size)
        len = size;

    std::memcpy(buf, password.data(), static_cast<std::size_t>(len));
    return len;
}

// SNI callback invoked by WolfSSL during handshake (server-side)
// Returns SNICbReturn enum: 0 = OK, fatal_return (2) = abort
static int
wolfssl_sni_callback(WOLFSSL* ssl, int* /* alert */, void* arg)
{
    void* sni_data = nullptr;
    unsigned short sni_len = wolfSSL_SNI_GetRequest(ssl, WOLFSSL_SNI_HOST_NAME, &sni_data);
    if(!sni_data || sni_len == 0)
        return 0;  // No SNI sent, continue

    std::string_view servername(static_cast<char const*>(sni_data), sni_len);

    auto* cd = static_cast<tls_context_data const*>(arg);
    if(cd && cd->servername_callback)
    {
        if(!cd->servername_callback(servername))
            return fatal_return;  // Callback rejected hostname
    }

    return 0;  // Accept
}

/** Cached WolfSSL contexts owning WOLFSSL_CTX for client and server.

    Created on first stream construction for a given tls_context,
    then reused for subsequent streams sharing that context.
    Maintains separate contexts for client and server roles since
    WolfSSL requires different method functions for each.
*/
class wolfssl_native_context
    : public native_context_base
{
public:
    WOLFSSL_CTX* client_ctx_;
    WOLFSSL_CTX* server_ctx_;

    static void
    apply_common_settings(WOLFSSL_CTX* ctx, tls_context_data const& cd)
    {
        if(!ctx)
            return;

        // Apply verify mode from config
        int verify_mode_flag = WOLFSSL_VERIFY_NONE;
        if(cd.verification_mode == tls_verify_mode::peer)
            verify_mode_flag = WOLFSSL_VERIFY_PEER;
        else if(cd.verification_mode == tls_verify_mode::require_peer)
            verify_mode_flag = WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        wolfSSL_CTX_set_verify(ctx, verify_mode_flag, nullptr);

        // Apply certificate chain if provided (entity cert + intermediates)
        // wolfSSL_CTX_use_certificate_chain_buffer loads entity as cert, rest as chain
        if(!cd.certificate_chain.empty())
        {
            wolfSSL_CTX_use_certificate_chain_buffer(ctx,
                reinterpret_cast<unsigned char const*>(cd.certificate_chain.data()),
                static_cast<long>(cd.certificate_chain.size()));
        }
        else if(!cd.entity_certificate.empty())
        {
            // Only use single certificate if no chain provided
            int format = (cd.entity_cert_format == tls_file_format::pem)
                ? WOLFSSL_FILETYPE_PEM : WOLFSSL_FILETYPE_ASN1;
            wolfSSL_CTX_use_certificate_buffer(ctx,
                reinterpret_cast<unsigned char const*>(cd.entity_certificate.data()),
                static_cast<long>(cd.entity_certificate.size()),
                format);
        }

        // Apply private key if provided
        if(!cd.private_key.empty())
        {
            // Set password callback before loading encrypted private key
            if(cd.password_callback)
            {
                wolfSSL_CTX_set_default_passwd_cb(ctx, wolfssl_password_callback);
                wolfSSL_CTX_set_default_passwd_cb_userdata(ctx,
                    const_cast<tls_context_data*>(&cd));
            }

            int format = (cd.private_key_format == tls_file_format::pem)
                ? WOLFSSL_FILETYPE_PEM : WOLFSSL_FILETYPE_ASN1;
            wolfSSL_CTX_use_PrivateKey_buffer(ctx,
                reinterpret_cast<unsigned char const*>(cd.private_key.data()),
                static_cast<long>(cd.private_key.size()),
                format);
        }

        // Apply CA certificates for verification
        for(auto const& ca : cd.ca_certificates)
        {
            wolfSSL_CTX_load_verify_buffer(ctx,
                reinterpret_cast<unsigned char const*>(ca.data()),
                static_cast<long>(ca.size()),
                WOLFSSL_FILETYPE_PEM);
        }

        // Apply verify depth
        wolfSSL_CTX_set_verify_depth(ctx, cd.verify_depth);
    }

    tls_context_data const* cd_;  // For SNI callback access

    explicit
    wolfssl_native_context(tls_context_data const& cd)
        : client_ctx_(nullptr)
        , server_ctx_(nullptr)
        , cd_(&cd)
    {
        // Create separate contexts for client and server
        client_ctx_ = wolfSSL_CTX_new(wolfTLS_client_method());
        server_ctx_ = wolfSSL_CTX_new(wolfTLS_server_method());

        apply_common_settings(client_ctx_, cd);
        apply_common_settings(server_ctx_, cd);

        // Set SNI callback on server context if provided
        if(server_ctx_ && cd.servername_callback)
        {
            wolfSSL_CTX_set_servername_callback(server_ctx_, wolfssl_sni_callback);
            wolfSSL_CTX_set_servername_arg(server_ctx_, const_cast<tls_context_data*>(&cd));
        }
    }

    ~wolfssl_native_context() override
    {
        if(client_ctx_)
            wolfSSL_CTX_free(client_ctx_);
        if(server_ctx_)
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
    auto* p = cd.find(&key, [&]
    {
        return new wolfssl_native_context(cd);
    });
    return static_cast<wolfssl_native_context*>(p);
}

} // namespace detail

//------------------------------------------------------------------------------

struct wolfssl_stream::impl
{
    capy::any_stream& s_;
    tls_context ctx_;
    WOLFSSL* ssl_ = nullptr;

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
    capy::coro_lock io_cm_;

    //--------------------------------------------------------------------------

    impl(capy::any_stream& s, tls_context ctx)
        : s_(s)
        , ctx_(std::move(ctx))
    {
        read_in_buf_.resize(default_buffer_size);
        read_out_buf_.resize(default_buffer_size);
        write_in_buf_.resize(default_buffer_size);
        write_out_buf_.resize(default_buffer_size);
    }

    ~impl()
    {
        if(ssl_)
            wolfSSL_free(ssl_);
        // WOLFSSL_CTX* is owned by cached native context, not freed here
    }

    //--------------------------------------------------------------------------
    // WolfSSL I/O Callbacks
    //--------------------------------------------------------------------------

    /** Callback invoked by WolfSSL when it needs to receive data.

        Returns data from the current operation's input buffer if available,
        otherwise returns WOLFSSL_CBIO_ERR_WANT_READ.
    */
    static int
    recv_callback(WOLFSSL*, char* buf, int sz, void* ctx)
    {
        auto* self = static_cast<impl*>(ctx);
        auto* op = self->current_op_;

        // Check if we have data in the input buffer
        std::size_t available = *op->in_len - *op->in_pos;
        if(available == 0)
        {
            // No data available, signal need to read
            op->want_read = true;
            return WOLFSSL_CBIO_ERR_WANT_READ;
        }

        // Copy available data to WolfSSL's buffer
        std::size_t to_copy = (std::min)(available, static_cast<std::size_t>(sz));
        std::memcpy(buf, op->in_buf->data() + *op->in_pos, to_copy);
        *op->in_pos += to_copy;

        // If we've consumed all data, reset buffer position
        if(*op->in_pos == *op->in_len)
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
    static int
    send_callback(WOLFSSL*, char* buf, int sz, void* ctx)
    {
        auto* self = static_cast<impl*>(ctx);
        auto* op = self->current_op_;

        // Check if we have room in the output buffer
        std::size_t available = op->out_buf->size() - *op->out_len;
        if(available == 0)
        {
            // Buffer full, signal need to write
            op->want_write = true;
            return WOLFSSL_CBIO_ERR_WANT_WRITE;
        }

        // Copy data to output buffer
        std::size_t to_copy = (std::min)(available, static_cast<std::size_t>(sz));
        std::memcpy(op->out_buf->data() + *op->out_len, buf, to_copy);
        *op->out_len += to_copy;

        // If we couldn't copy everything, signal partial write
        if(to_copy < static_cast<std::size_t>(sz))
            op->want_write = true;

        return static_cast<int>(to_copy);
    }

    //--------------------------------------------------------------------------
    // Inner coroutines for TLS read/write operations
    //--------------------------------------------------------------------------

    capy::io_task<std::size_t>
    do_read_some(capy::some_mutable_buffers buffers)
    {
        std::error_code ec;
        std::size_t total_read = 0;

        // Set up operation buffers for callbacks
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        for(auto& buf : buffers)
        {
            char* dest = static_cast<char*>(buf.data());
            int remaining = static_cast<int>(buf.size());

            while(remaining > 0)
            {
                op.want_read = false;
                op.want_write = false;

                int ret = wolfSSL_read(ssl_, dest, remaining);

                if(ret > 0)
                {
                    // Successfully read some data
                    dest += ret;
                    remaining -= ret;
                    total_read += static_cast<std::size_t>(ret);

                    // For read_some semantics, return after first successful read
                    if(total_read > 0)
                    {
                        current_op_ = nullptr;
                        co_return {std::error_code{}, total_read};
                    }
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if(err == WOLFSSL_ERROR_WANT_READ)
                    {
                        if(read_in_pos_ == read_in_len_)
                        {
                            read_in_pos_ = 0;
                            read_in_len_ = 0;
                        }
                        capy::mutable_buffer rbuf(
                            read_in_buf_.data() + read_in_len_,
                            read_in_buf_.size() - read_in_len_);

                        auto guard = co_await io_cm_.scoped_lock();
                        auto [rec, rn] = co_await s_.read_some(rbuf);
                        if(rec)
                        {
                            if(rec == make_error_code(capy::error::eof))
                            {
                                // Check if we got a proper TLS shutdown
                                if(wolfSSL_get_shutdown(ssl_) & SSL_RECEIVED_SHUTDOWN)
                                    ec = make_error_code(capy::error::eof);
                                else
                                    ec = make_error_code(capy::error::stream_truncated);
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
                    else if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Renegotiation
                        if(read_out_len_ > 0)
                        {
                            auto guard = co_await io_cm_.scoped_lock();
                            auto [wec, wn] = co_await capy::write(s_,
                                capy::const_buffer(read_out_buf_.data(), read_out_len_));
                            read_out_len_ = 0;
                            if(wec)
                            {
                                current_op_ = nullptr;
                                co_return {wec, total_read};
                            }
                        }
                    }
                    else if(err == WOLFSSL_ERROR_ZERO_RETURN)
                    {
                        // Clean TLS shutdown - treat as EOF
                        current_op_ = nullptr;
                        co_return {make_error_code(capy::error::eof), total_read};
                    }
                    else
                    {
                        // Other error
                        current_op_ = nullptr;
                        co_return {std::error_code(err, std::system_category()), total_read};
                    }
                }
            }
        }

        current_op_ = nullptr;
        co_return {std::error_code{}, total_read};
    }

    capy::io_task<std::size_t>
    do_write_some(capy::some_const_buffers buffers)
    {
        std::error_code ec;
        std::size_t total_written = 0;

        // Set up operation buffers for callbacks
        op_buffers op{
            &write_in_buf_, &write_in_pos_, &write_in_len_,
            &write_out_buf_, &write_out_len_,
            false, false
        };
        current_op_ = &op;

        for(auto const& buf : buffers)
        {
            char const* src = static_cast<char const*>(buf.data());
            int remaining = static_cast<int>(buf.size());

            while(remaining > 0)
            {
                op.want_read = false;
                op.want_write = false;

                int ret = wolfSSL_write(ssl_, src, remaining);

                if(ret > 0)
                {
                    // Successfully wrote some data
                    src += ret;
                    remaining -= ret;
                    total_written += static_cast<std::size_t>(ret);

                    // For write_some semantics, return after first successful write
                    if(total_written > 0)
                    {
                        // Flush any pending output
                        if(write_out_len_ > 0)
                        {
                            auto guard = co_await io_cm_.scoped_lock();
                            auto [wec, wn] = co_await capy::write(s_,
                                capy::const_buffer(write_out_buf_.data(), write_out_len_));
                            write_out_len_ = 0;
                            if(wec)
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

                    if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        if(write_out_len_ > 0)
                        {
                            auto guard = co_await io_cm_.scoped_lock();
                            auto [wec, wn] = co_await capy::write(s_,
                                capy::const_buffer(write_out_buf_.data(), write_out_len_));
                            write_out_len_ = 0;
                            if(wec)
                            {
                                current_op_ = nullptr;
                                co_return {wec, total_written};
                            }
                        }
                    }
                    else if(err == WOLFSSL_ERROR_WANT_READ)
                    {
                        // Renegotiation
                        if(write_in_pos_ == write_in_len_)
                        {
                            write_in_pos_ = 0;
                            write_in_len_ = 0;
                        }
                        capy::mutable_buffer rbuf(
                            write_in_buf_.data() + write_in_len_,
                            write_in_buf_.size() - write_in_len_);
                        auto guard = co_await io_cm_.scoped_lock();
                        auto [rec, rn] = co_await s_.read_some(rbuf);
                        if(rec)
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
                        co_return {std::error_code(err, std::system_category()), total_written};
                    }
                }
            }
        }

        current_op_ = nullptr;
        co_return {std::error_code{}, total_written};
    }

    capy::io_task<>
    do_handshake(int type)
    {
        std::error_code ec;

        // Initialize SSL object for the specified role (deferred from construction)
        ec = init_ssl_for_role(type);
        if(ec)
            co_return {ec};

        // Set up operation buffers for callbacks (use read buffers for handshake)
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        while(true)
        {
            op.want_read = false;
            op.want_write = false;

            // Call appropriate handshake function based on type
            int ret;
            if(type == wolfssl_stream::client)
                ret = wolfSSL_connect(ssl_);
            else
                ret = wolfSSL_accept(ssl_);

            if(ret == WOLFSSL_SUCCESS)
            {
                // Handshake completed successfully
                // Flush any remaining output
                if(read_out_len_ > 0)
                {
                    auto guard = co_await io_cm_.scoped_lock();
                    auto [wec, wn] = co_await capy::write(s_,
                        capy::const_buffer(read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if(wec)
                        ec = wec;
                }
                break;
            }
            else
            {
                int err = wolfSSL_get_error(ssl_, ret);

                if(err == WOLFSSL_ERROR_WANT_READ)
                {
                    // Must flush (e.g. ClientHello) before reading ServerHello
                    if(read_out_len_ > 0)
                    {
                        auto guard = co_await io_cm_.scoped_lock();
                        auto [wec, wn] = co_await capy::write(s_,
                            capy::const_buffer(read_out_buf_.data(), read_out_len_));
                        read_out_len_ = 0;
                        if(wec)
                        {
                            ec = wec;
                            break;
                        }
                    }

                    if(read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer rbuf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto guard = co_await io_cm_.scoped_lock();
                    auto [rec, rn] = co_await s_.read_some(rbuf);
                    if(rec)
                    {
                        ec = rec;
                        break;
                    }
                    read_in_len_ += rn;
                }
                else if(err == WOLFSSL_ERROR_WANT_WRITE)
                {
                    if(read_out_len_ > 0)
                    {
                        auto guard = co_await io_cm_.scoped_lock();
                        auto [wec, wn] = co_await capy::write(s_,
                            capy::const_buffer(read_out_buf_.data(), read_out_len_));
                        read_out_len_ = 0;
                        if(wec)
                        {
                            ec = wec;
                            break;
                        }
                    }
                }
                else
                {
                    // Other error
                    ec = std::error_code(err, std::system_category());
                    break;
                }
            }
        }

        current_op_ = nullptr;
        co_return {ec};
    }

    capy::io_task<>
    do_shutdown()
    {
        std::error_code ec;

        // Set up operation buffers for callbacks (use read buffers for shutdown)
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        while(true)
        {
            op.want_read = false;
            op.want_write = false;

            int ret = wolfSSL_shutdown(ssl_);
            int err = (ret != WOLFSSL_SUCCESS) ? wolfSSL_get_error(ssl_, ret) : 0;

            if(ret == WOLFSSL_SUCCESS)
            {
                // Bidirectional shutdown complete - flush any remaining output
                if(read_out_len_ > 0)
                {
                    auto guard = co_await io_cm_.scoped_lock();
                    auto [wec, wn] = co_await capy::write(s_,
                        capy::const_buffer(read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if(wec)
                        ec = wec;
                }
                break;
            }
            else if(ret == WOLFSSL_SHUTDOWN_NOT_DONE)
            {
                // First, flush any pending output (sends our close_notify)
                if(read_out_len_ > 0)
                {
                    auto guard = co_await io_cm_.scoped_lock();
                    auto [wec, wn] = co_await capy::write(s_,
                        capy::const_buffer(read_out_buf_.data(), read_out_len_));
                    read_out_len_ = 0;
                    if(wec)
                        break;  // Socket error during shutdown write - acceptable
                }

                // Check what WolfSSL needs next
                if(err == WOLFSSL_ERROR_WANT_READ || err == 0)
                {
                    if(read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer rbuf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto guard = co_await io_cm_.scoped_lock();
                    auto [rec, rn] = co_await s_.read_some(rbuf);
                    if(rec)
                        break;  // EOF or socket error during shutdown read - acceptable
                    read_in_len_ += rn;
                }
                else if(err == WOLFSSL_ERROR_WANT_WRITE)
                {
                    // Just need to flush more - already done above, continue loop
                }
                else if(err == WOLFSSL_ERROR_SYSCALL || err == SSL_ERROR_ZERO_RETURN)
                {
                    // Socket closed or peer sent close_notify - shutdown complete
                    break;
                }
                else
                {
                    // Unexpected error
                    ec = std::error_code(err, std::system_category());
                    break;
                }
            }
            else
            {
                // SSL_FATAL_ERROR or negative return
                ec = std::error_code(err, std::system_category());
                break;
            }
        }

        current_op_ = nullptr;
        co_return {ec};
    }

    //--------------------------------------------------------------------------
    // Initialization
    //--------------------------------------------------------------------------

    std::error_code
    init_ssl_for_role(int type)
    {
        // Already initialized?
        if(ssl_)
            return {};

        // Get cached native contexts from tls_context
        auto& cd = detail::get_tls_context_data(ctx_);
        auto* native = detail::get_wolfssl_native_context(cd);
        if(!native)
        {
            return std::error_code(
                wolfSSL_get_error(nullptr, 0),
                std::system_category());
        }

        // Select appropriate context based on role
        WOLFSSL_CTX* native_ctx = (type == wolfssl_stream::client)
            ? native->client_ctx_
            : native->server_ctx_;

        if(!native_ctx)
        {
            return std::error_code(
                wolfSSL_get_error(nullptr, 0),
                std::system_category());
        }

        // Create SSL session from the role-specific context
        ssl_ = wolfSSL_new(native_ctx);
        if(!ssl_)
        {
            int err = wolfSSL_get_error(nullptr, 0);
            return std::error_code(err, std::system_category());
        }

        // Set custom I/O callbacks
        wolfSSL_SSLSetIORecv(ssl_, &recv_callback);
        wolfSSL_SSLSetIOSend(ssl_, &send_callback);

        // Set this impl as the I/O context
        wolfSSL_SetIOReadCtx(ssl_, this);
        wolfSSL_SetIOWriteCtx(ssl_, this);

        // Apply per-session config (SNI + hostname verification) from context
        if(type == wolfssl_stream::client && !cd.hostname.empty())
        {
            // Set SNI extension so server knows which cert to present
            wolfSSL_UseSNI(ssl_, WOLFSSL_SNI_HOST_NAME,
                cd.hostname.data(),
                static_cast<unsigned short>(cd.hostname.size()));

            // Enable hostname verification (checks CN/SAN in peer cert)
            wolfSSL_check_domain_name(ssl_, cd.hostname.c_str());
        }

        return {};
    }
};

//------------------------------------------------------------------------------

wolfssl_stream::impl*
wolfssl_stream::
make_impl(capy::any_stream& stream, tls_context const& ctx)
{
    // SSL object creation is deferred to handshake time when we know the role
    return new impl(stream, ctx);
}

wolfssl_stream::
~wolfssl_stream()
{
    delete impl_;
}

wolfssl_stream::
wolfssl_stream(wolfssl_stream&& other) noexcept
    : stream_(std::move(other.stream_))
    , impl_(other.impl_)
{
    other.impl_ = nullptr;
}

wolfssl_stream&
wolfssl_stream::
operator=(wolfssl_stream&& other) noexcept
{
    if(this != &other)
    {
        delete impl_;
        stream_ = std::move(other.stream_);
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

capy::io_task<std::size_t>
wolfssl_stream::
do_read_some(capy::some_mutable_buffers buffers)
{
    co_return co_await impl_->do_read_some(buffers);
}

capy::io_task<std::size_t>
wolfssl_stream::
do_write_some(capy::some_const_buffers buffers)
{
    co_return co_await impl_->do_write_some(buffers);
}

capy::io_task<>
wolfssl_stream::
handshake(handshake_type type)
{
    co_return co_await impl_->do_handshake(type);
}

capy::io_task<>
wolfssl_stream::
shutdown()
{
    co_return co_await impl_->do_shutdown();
}

} // namespace boost::corosio
