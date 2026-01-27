//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tls/wolfssl_stream.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/task.hpp>

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

    TLS layer wrapping an underlying io_stream. Supports one concurrent
    read_some and one concurrent write_some (like Asio's ssl::stream).

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

    This design allows a single tls::context to be shared across both client
    and server streams without requiring OpenSSL compatibility mode in WolfSSL.

    Key Types
    ---------
    - wolfssl_stream_impl_ : tls_stream_impl  -- the impl stored in io_object::impl_
    - wolfssl_native_context                  -- caches client_ctx_ and server_ctx_
    - recv_callback, send_callback            -- WolfSSL I/O hooks (static)
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

// SNI callback invoked by WolfSSL during handshake (server-side)
// Returns SNICbReturn enum: 0 = OK, fatal_return (2) = abort
static int
wolfssl_sni_callback( WOLFSSL* ssl, int* /* alert */, void* arg )
{
    void* sni_data = nullptr;
    unsigned short sni_len = wolfSSL_SNI_GetRequest( ssl, WOLFSSL_SNI_HOST_NAME, &sni_data );
    if( !sni_data || sni_len == 0 )
        return 0;  // No SNI sent, continue

    std::string_view servername( static_cast<char const*>( sni_data ), sni_len );

    auto* cd = static_cast<context_data const*>( arg );
    if( cd && cd->servername_callback )
    {
        if( !cd->servername_callback( servername ) )
            return fatal_return;  // Callback rejected hostname
    }

    return 0;  // Accept
}

/** Cached WolfSSL contexts owning WOLFSSL_CTX for client and server.

    Created on first stream construction for a given tls::context,
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
    apply_common_settings( WOLFSSL_CTX* ctx, context_data const& cd )
    {
        if( !ctx )
            return;

        // Apply verify mode from config
        int verify_mode_flag = WOLFSSL_VERIFY_NONE;
        if( cd.verification_mode == verify_mode::peer )
            verify_mode_flag = WOLFSSL_VERIFY_PEER;
        else if( cd.verification_mode == verify_mode::require_peer )
            verify_mode_flag = WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        wolfSSL_CTX_set_verify( ctx, verify_mode_flag, nullptr );

        // Apply certificate chain if provided (entity cert + intermediates)
        // wolfSSL_CTX_use_certificate_chain_buffer loads entity as cert, rest as chain
        if( !cd.certificate_chain.empty() )
        {
            wolfSSL_CTX_use_certificate_chain_buffer( ctx,
                reinterpret_cast<unsigned char const*>( cd.certificate_chain.data() ),
                static_cast<long>( cd.certificate_chain.size() ) );
        }
        else if( !cd.entity_certificate.empty() )
        {
            // Only use single certificate if no chain provided
            int format = ( cd.entity_cert_format == file_format::pem )
                ? WOLFSSL_FILETYPE_PEM : WOLFSSL_FILETYPE_ASN1;
            wolfSSL_CTX_use_certificate_buffer( ctx,
                reinterpret_cast<unsigned char const*>( cd.entity_certificate.data() ),
                static_cast<long>( cd.entity_certificate.size() ),
                format );
        }

        // Apply private key if provided
        if( !cd.private_key.empty() )
        {
            int format = ( cd.private_key_format == file_format::pem )
                ? WOLFSSL_FILETYPE_PEM : WOLFSSL_FILETYPE_ASN1;
            wolfSSL_CTX_use_PrivateKey_buffer( ctx,
                reinterpret_cast<unsigned char const*>( cd.private_key.data() ),
                static_cast<long>( cd.private_key.size() ),
                format );
        }

        // Apply CA certificates for verification
        for( auto const& ca : cd.ca_certificates )
        {
            wolfSSL_CTX_load_verify_buffer( ctx,
                reinterpret_cast<unsigned char const*>( ca.data() ),
                static_cast<long>( ca.size() ),
                WOLFSSL_FILETYPE_PEM );
        }

        // Apply verify depth
        wolfSSL_CTX_set_verify_depth( ctx, cd.verify_depth );
    }

    context_data const* cd_;  // For SNI callback access

    explicit
    wolfssl_native_context( context_data const& cd )
        : client_ctx_( nullptr )
        , server_ctx_( nullptr )
        , cd_( &cd )
    {
        // Create separate contexts for client and server
        client_ctx_ = wolfSSL_CTX_new( wolfTLS_client_method() );
        server_ctx_ = wolfSSL_CTX_new( wolfTLS_server_method() );

        apply_common_settings( client_ctx_, cd );
        apply_common_settings( server_ctx_, cd );

        // Set SNI callback on server context if provided
        if( server_ctx_ && cd.servername_callback )
        {
            wolfSSL_CTX_set_servername_callback( server_ctx_, wolfssl_sni_callback );
            wolfSSL_CTX_set_servername_arg( server_ctx_, const_cast<context_data*>( &cd ) );
        }
    }

    ~wolfssl_native_context() override
    {
        if( client_ctx_ )
            wolfSSL_CTX_free( client_ctx_ );
        if( server_ctx_ )
            wolfSSL_CTX_free( server_ctx_ );
    }
};

/** Get or create cached wolfssl_native_context for this context.

    @param cd The context implementation.

    @return Pointer to the cached native context wrapper.
*/
inline wolfssl_native_context*
get_wolfssl_native_context( context_data const& cd )
{
    static char key;
    auto* p = cd.find( &key, [&]
    {
        return new wolfssl_native_context( cd );
    });
    return static_cast<wolfssl_native_context*>( p );
}

} // namespace tls::detail

//------------------------------------------------------------------------------

struct wolfssl_stream_impl_
    : tls_stream::tls_stream_impl
{
    io_stream& s_;
    tls::context ctx_;      // holds ref to cached native context
    WOLFSSL* ssl_ = nullptr;

    // Buffers for read operations (used by do_read_some)
    std::vector<char> read_in_buf_;
    std::size_t read_in_pos_ = 0;
    std::size_t read_in_len_ = 0;
    std::vector<char> read_out_buf_;
    std::size_t read_out_len_ = 0;

    // Buffers for write operations (used by do_write_some)
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
    capy::async_mutex io_mutex_;

    //--------------------------------------------------------------------------

    wolfssl_stream_impl_( io_stream& s, tls::context ctx )
        : s_( s )
        , ctx_( std::move( ctx ) )
    {
        read_in_buf_.resize( default_buffer_size );
        read_out_buf_.resize( default_buffer_size );
        write_in_buf_.resize( default_buffer_size );
        write_out_buf_.resize( default_buffer_size );
    }

    ~wolfssl_stream_impl_()
    {
        if( ssl_ )
            wolfSSL_free( ssl_ );
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
        auto* impl = static_cast<wolfssl_stream_impl_*>(ctx);
        auto* op = impl->current_op_;

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
        auto* impl = static_cast<wolfssl_stream_impl_*>(ctx);
        auto* op = impl->current_op_;

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

    capy::task<capy::io_result<std::size_t>>
    do_underlying_read(capy::mutable_buffer buf, std::stop_token token)
    {
        if(token.stop_requested())
            co_return capy::io_result<std::size_t>{
                make_error_code(system::errc::operation_canceled), 0};

        auto guard = co_await io_mutex_.scoped_lock();
        co_return co_await s_.read_some(buf);
    }

    capy::task<capy::io_result<std::size_t>>
    do_underlying_write(capy::mutable_buffer buf, std::stop_token token)
    {
        if(token.stop_requested())
            co_return capy::io_result<std::size_t>{
                make_error_code(system::errc::operation_canceled), 0};

        auto guard = co_await io_mutex_.scoped_lock();
        co_return co_await s_.write_some(buf);
    }

    //--------------------------------------------------------------------------
    // Inner coroutines for TLS read/write operations
    //--------------------------------------------------------------------------

    /** Inner coroutine that performs TLS read with WANT_READ loop.

        Calls wolfSSL_read in a loop, performing async reads from the
        underlying stream when needed.
    */
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

        // Set up operation buffers for callbacks
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        // Process each destination buffer
        for(std::size_t i = 0; i < buf_count && !token.stop_requested(); ++i)
        {
            char* dest = static_cast<char*>(dest_bufs[i].data());
            int remaining = static_cast<int>(dest_bufs[i].size());

            while(remaining > 0 && !token.stop_requested())
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
                        goto done;
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if(err == WOLFSSL_ERROR_WANT_READ)
                    {
                        if(read_in_pos_ == read_in_len_) { read_in_pos_ = 0; read_in_len_ = 0; }
                        capy::mutable_buffer buf(read_in_buf_.data() + read_in_len_, read_in_buf_.size() - read_in_len_);
                        auto [rec, rn] = co_await do_underlying_read(buf, token);
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
                            goto done;
                        }
                        read_in_len_ += rn;
                    }
                    else if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Renegotiation
                        while(read_out_len_ > 0)
                        {
                            capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                            auto [wec, wn] = co_await do_underlying_write(buf, token);
                            if(wec) { ec = wec; goto done; }
                            if(wn < read_out_len_)
                                std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                            read_out_len_ -= wn;
                        }
                    }
                    else if(err == WOLFSSL_ERROR_ZERO_RETURN)
                    {
                        // Clean TLS shutdown - treat as EOF
                        ec = make_error_code(capy::error::eof);
                        goto done;
                    }
                    else
                    {
                        // Other error
                        ec = system::error_code(err, system::system_category());
                        goto done;
                    }
                }
            }
        }

    done:
        current_op_ = nullptr;

        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;
        *bytes_out = total_read;

        // Resume the original caller via executor
        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    /** Inner coroutine that performs TLS write with WANT_WRITE loop.

        Calls wolfSSL_write in a loop, performing async writes to the
        underlying stream when needed.
    */
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

        // Set up operation buffers for callbacks
        op_buffers op{
            &write_in_buf_, &write_in_pos_, &write_in_len_,
            &write_out_buf_, &write_out_len_,
            false, false
        };
        current_op_ = &op;

        // Process each source buffer
        for(std::size_t i = 0; i < buf_count && !token.stop_requested(); ++i)
        {
            char const* src = static_cast<char const*>(src_bufs[i].data());
            int remaining = static_cast<int>(src_bufs[i].size());

            while(remaining > 0 && !token.stop_requested())
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
                        while(write_out_len_ > 0)
                        {
                            capy::mutable_buffer buf(write_out_buf_.data(), write_out_len_);
                            auto [wec, wn] = co_await do_underlying_write(buf, token);
                            if(wec) { ec = wec; goto done; }
                            if(wn < write_out_len_)
                                std::memmove(write_out_buf_.data(), write_out_buf_.data() + wn, write_out_len_ - wn);
                            write_out_len_ -= wn;
                        }
                        goto done;
                    }
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        while(write_out_len_ > 0)
                        {
                            capy::mutable_buffer buf(write_out_buf_.data(), write_out_len_);
                            auto [wec, wn] = co_await do_underlying_write(buf, token);
                            if(wec) { ec = wec; goto done; }
                            if(wn < write_out_len_)
                                std::memmove(write_out_buf_.data(), write_out_buf_.data() + wn, write_out_len_ - wn);
                            write_out_len_ -= wn;
                        }
                    }
                    else if(err == WOLFSSL_ERROR_WANT_READ)
                    {
                        // Renegotiation
                        if(write_in_pos_ == write_in_len_) { write_in_pos_ = 0; write_in_len_ = 0; }
                        capy::mutable_buffer buf(write_in_buf_.data() + write_in_len_, write_in_buf_.size() - write_in_len_);
                        auto [rec, rn] = co_await do_underlying_read(buf, token);
                        if(rec) { ec = rec; goto done; }
                        write_in_len_ += rn;
                    }
                    else
                    {
                        // Other error
                        ec = system::error_code(err, system::system_category());
                        goto done;
                    }
                }
            }
        }

    done:
        current_op_ = nullptr;

        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;
        *bytes_out = total_written;

        // Resume the original caller via executor
        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    /** Inner coroutine that performs TLS handshake with WANT_READ/WANT_WRITE loop.

        Calls wolfSSL_connect (client) or wolfSSL_accept (server) in a loop,
        performing async I/O on the underlying stream when needed.
    */
    capy::task<>
    do_handshake(
        int type,
        std::stop_token token,
        system::error_code* ec_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;

        // Initialize SSL object for the specified role (deferred from construction)
        ec = init_ssl_for_role( type );
        if( ec )
        {
            *ec_out = ec;
            current_op_ = nullptr;
            d.dispatch(capy::coro{continuation}).resume();
            co_return;
        }

        // Set up operation buffers for callbacks (use read buffers for handshake)
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        while(!token.stop_requested())
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
                while(read_out_len_ > 0)
                {
                    capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                    auto [wec, wn] = co_await do_underlying_write(buf, token);
                    if(wec)
                    {
                        ec = wec;
                        break;
                    }
                    if(wn < read_out_len_)
                        std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                    read_out_len_ -= wn;
                }
                break;
            }
            else
            {
                int err = wolfSSL_get_error(ssl_, ret);

                if(err == WOLFSSL_ERROR_WANT_READ)
                {
                    // Must flush (e.g. ClientHello) before reading ServerHello
                    while(read_out_len_ > 0)
                    {
                        capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                        auto [wec, wn] = co_await do_underlying_write(buf, token);
                        if(wec)
                        {
                            ec = wec;
                            goto exit_loop;
                        }
                        if(wn < read_out_len_)
                            std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                        read_out_len_ -= wn;
                    }

                    if(read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer buf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto [rec, rn] = co_await do_underlying_read(buf, token);
                    if(rec)
                    {
                        ec = rec;
                        break;
                    }
                    read_in_len_ += rn;
                }
                else if(err == WOLFSSL_ERROR_WANT_WRITE)
                {
                    while(read_out_len_ > 0)
                    {
                        capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                        auto [wec, wn] = co_await do_underlying_write(buf, token);
                        if(wec)
                        {
                            ec = wec;
                            goto exit_loop;
                        }
                        if(wn < read_out_len_)
                            std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                        read_out_len_ -= wn;
                    }
                }
                else
                {
                    // Other error
                    ec = system::error_code(err, system::system_category());
                    break;
                }
            }
        }

    exit_loop:
        current_op_ = nullptr;

        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;

        // Resume the original caller via executor
        d.dispatch(capy::coro{continuation}).resume();
        co_return;
    }

    /** Inner coroutine that performs TLS shutdown with WANT_READ/WANT_WRITE loop.

        Calls wolfSSL_shutdown in a loop, performing async I/O on the
        underlying stream when needed.
    */
    capy::task<>
    do_shutdown(
        std::stop_token token,
        system::error_code* ec_out,
        std::coroutine_handle<> continuation,
        capy::executor_ref d)
    {
        system::error_code ec;

        // Set up operation buffers for callbacks (use read buffers for shutdown)
        op_buffers op{
            &read_in_buf_, &read_in_pos_, &read_in_len_,
            &read_out_buf_, &read_out_len_,
            false, false
        };
        current_op_ = &op;

        // TLS shutdown is bidirectional:
        // 1. We send close_notify to peer
        // 2. We receive peer's close_notify
        // wolfSSL_shutdown returns WOLFSSL_SHUTDOWN_NOT_DONE until both complete.
        // After sending our close_notify (via flush), we must read from the socket
        // to receive the peer's close_notify, matching OpenSSL's behavior.

        while(!token.stop_requested())
        {
            op.want_read = false;
            op.want_write = false;

            int ret = wolfSSL_shutdown(ssl_);
            // Only call wolfSSL_get_error once per iteration to avoid consuming error state
            int err = (ret != WOLFSSL_SUCCESS) ? wolfSSL_get_error(ssl_, ret) : 0;

            if(ret == WOLFSSL_SUCCESS)
            {
                // Bidirectional shutdown complete - flush any remaining output
                while(read_out_len_ > 0)
                {
                    capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                    auto [wec, wn] = co_await do_underlying_write(buf, token);
                    if(wec)
                    {
                        ec = wec;
                        break;
                    }
                    if(wn < read_out_len_)
                        std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                    read_out_len_ -= wn;
                }
                break;
            }
            else if(ret == WOLFSSL_SHUTDOWN_NOT_DONE)
            {
                // Sent our close_notify (or need to), waiting for peer's close_notify
                // This mirrors OpenSSL's SSL_shutdown() returning 0.
                
                // First, flush any pending output (sends our close_notify)
                while(read_out_len_ > 0)
                {
                    capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                    auto [wec, wn] = co_await do_underlying_write(buf, token);
                    if(wec)
                    {
                        // Socket error during shutdown write - acceptable, we're done
                        goto exit_shutdown;
                    }
                    if(wn < read_out_len_)
                        std::memmove(read_out_buf_.data(), read_out_buf_.data() + wn, read_out_len_ - wn);
                    read_out_len_ -= wn;
                }

                // Check what WolfSSL needs next
                if(err == WOLFSSL_ERROR_WANT_READ || err == 0)
                {
                    // Need to read peer's close_notify from the socket
                    // err==0 also needs a read - the close_notify was sent, now wait for peer
                    if(read_in_pos_ == read_in_len_)
                    {
                        read_in_pos_ = 0;
                        read_in_len_ = 0;
                    }
                    capy::mutable_buffer buf(
                        read_in_buf_.data() + read_in_len_,
                        read_in_buf_.size() - read_in_len_);
                    auto [rec, rn] = co_await do_underlying_read(buf, token);
                    if(rec)
                    {
                        // EOF or socket error during shutdown read - acceptable
                        // The peer may have just closed the socket without close_notify
                        goto exit_shutdown;
                    }
                    read_in_len_ += rn;
                    // Continue loop to process the received data with wolfSSL_shutdown
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
                    ec = system::error_code(err, system::system_category());
                    break;
                }
            }
            else
            {
                // SSL_FATAL_ERROR or negative return
                ec = system::error_code(err, system::system_category());
                break;
            }
        }

    exit_shutdown:
        current_op_ = nullptr;

        if(token.stop_requested())
            ec = make_error_code(system::errc::operation_canceled);

        *ec_out = ec;

        // Resume the original caller via executor
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
        // Extract buffers from type-erased parameter
        // Pass by value so array is copied into coroutine frame
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        // Launch inner coroutine via run_async with stop_token for cancellation
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
        // Extract buffers from type-erased parameter
        // Pass by value so array is copied into coroutine frame
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        // Launch inner coroutine via run_async with stop_token for cancellation
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
        // Launch inner coroutine via run_async with stop_token for cancellation
        capy::run_async(d, token)(
            do_handshake(type, token, ec, h, d));
    }

    void shutdown(
        std::coroutine_handle<> h,
        capy::executor_ref d,
        std::stop_token token,
        system::error_code* ec) override
    {
        // Launch inner coroutine via run_async with stop_token for cancellation
        capy::run_async(d, token)(
            do_shutdown(token, ec, h, d));
    }

    //--------------------------------------------------------------------------
    // Initialization
    //--------------------------------------------------------------------------

    /** Initialize SSL object for the specified role.

        @param type wolfssl_stream::client or wolfssl_stream::server
        @return Error code if initialization failed.
    */
    system::error_code
    init_ssl_for_role( int type )
    {
        // Already initialized?
        if( ssl_ )
            return {};

        // Get cached native contexts from tls::context
        auto& impl = tls::detail::get_context_data( ctx_ );
        auto* native = tls::detail::get_wolfssl_native_context( impl );
        if( !native )
        {
            return system::error_code(
                wolfSSL_get_error( nullptr, 0 ),
                system::system_category() );
        }

        // Select appropriate context based on role
        WOLFSSL_CTX* native_ctx = ( type == wolfssl_stream::client )
            ? native->client_ctx_
            : native->server_ctx_;

        if( !native_ctx )
        {
            return system::error_code(
                wolfSSL_get_error( nullptr, 0 ),
                system::system_category() );
        }

        // Create SSL session from the role-specific context
        ssl_ = wolfSSL_new( native_ctx );
        if( !ssl_ )
        {
            int err = wolfSSL_get_error( nullptr, 0 );
            return system::error_code( err, system::system_category() );
        }

        // Set custom I/O callbacks
        wolfSSL_SSLSetIORecv( ssl_, &recv_callback );
        wolfSSL_SSLSetIOSend( ssl_, &send_callback );

        // Set this impl as the I/O context
        wolfSSL_SetIOReadCtx( ssl_, this );
        wolfSSL_SetIOWriteCtx( ssl_, this );

        // Apply per-session config (SNI + hostname verification) from context
        if( type == wolfssl_stream::client && !impl.hostname.empty() )
        {
            // Set SNI extension so server knows which cert to present
            wolfSSL_UseSNI( ssl_, WOLFSSL_SNI_HOST_NAME,
                impl.hostname.data(),
                static_cast<unsigned short>( impl.hostname.size() ) );

            // Enable hostname verification (checks CN/SAN in peer cert)
            wolfSSL_check_domain_name( ssl_, impl.hostname.c_str() );
        }

        return {};
    }
};

//------------------------------------------------------------------------------

wolfssl_stream::
wolfssl_stream( io_stream& stream, tls::context ctx )
    : tls_stream( stream )
{
    // SSL object creation is deferred to handshake time when we know the role
    impl_ = new wolfssl_stream_impl_( s_, std::move( ctx ) );
}

wolfssl_stream::
~wolfssl_stream()
{
    if( impl_ )
        impl_->release();
}

} // namespace boost::corosio
