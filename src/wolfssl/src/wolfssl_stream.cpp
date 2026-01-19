//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/capy/ex/async_mutex.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/task.hpp>

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

    Key Types
    ---------
    - wolfssl_stream_impl : io_stream_impl  -- the impl stored in io_object::impl_
    - recv_callback, send_callback          -- WolfSSL I/O hooks (static)
    - do_read_some, do_write_some           -- inner coroutines with WANT_* loops
*/

namespace boost {
namespace corosio {

namespace {

// Default buffer size for TLS I/O
constexpr std::size_t default_buffer_size = 16384;

// Maximum number of buffers to handle in a single operation
constexpr std::size_t max_buffers = 8;

// Buffer array type for coroutine parameters (copied into frame)
using buffer_array = std::array<capy::mutable_buffer, max_buffers>;

} // namespace

//------------------------------------------------------------------------------

struct wolfssl_stream_impl_
    : wolfssl_stream::wolfssl_stream_impl
{
    io_stream& s_;
    WOLFSSL_CTX* ctx_ = nullptr;
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

    explicit
    wolfssl_stream_impl_(io_stream& s)
        : s_(s)
    {
        read_in_buf_.resize(default_buffer_size);
        read_out_buf_.resize(default_buffer_size);
        write_in_buf_.resize(default_buffer_size);
        write_out_buf_.resize(default_buffer_size);
    }

    ~wolfssl_stream_impl_()
    {
        if(ssl_)
            wolfSSL_free(ssl_);
        if(ctx_)
            wolfSSL_CTX_free(ctx_);
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

    capy::task<io_result<std::size_t>>
    do_underlying_read(capy::mutable_buffer buf)
    {
        auto guard = co_await io_mutex_.scoped_lock();
        co_return co_await s_.read_some(buf);
    }

    capy::task<io_result<std::size_t>>
    do_underlying_write(capy::mutable_buffer buf)
    {
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
        capy::any_executor_ref d)
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
                        auto [rec, rn] = co_await do_underlying_read(buf);
                        if(rec) { ec = rec; goto done; }
                        read_in_len_ += rn;
                    }
                    else if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Renegotiation
                        while(read_out_len_ > 0)
                        {
                            capy::mutable_buffer buf(read_out_buf_.data(), read_out_len_);
                            auto [wec, wn] = co_await do_underlying_write(buf);
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
        d.dispatch(capy::any_coro{continuation}).resume();
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
        capy::any_executor_ref d)
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
                            auto [wec, wn] = co_await do_underlying_write(buf);
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
                            auto [wec, wn] = co_await do_underlying_write(buf);
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
                        auto [rec, rn] = co_await do_underlying_read(buf);
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
        d.dispatch(capy::any_coro{continuation}).resume();
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
        capy::any_executor_ref d)
    {
        system::error_code ec;

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
                    auto [wec, wn] = co_await do_underlying_write(buf);
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
                        auto [wec, wn] = co_await do_underlying_write(buf);
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
                    auto [rec, rn] = co_await do_underlying_read(buf);
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
                        auto [wec, wn] = co_await do_underlying_write(buf);
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
        d.dispatch(capy::any_coro{continuation}).resume();
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
        capy::any_executor_ref d,
        capy::any_bufref& param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        // Extract buffers from type-erased parameter
        // Pass by value so array is copied into coroutine frame
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        // Launch inner coroutine via run_async
        capy::run_async(d)(
            do_read_some(bufs, count, token, ec, bytes, h, d));
    }

    void write_some(
        std::coroutine_handle<> h,
        capy::any_executor_ref d,
        capy::any_bufref& param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        // Extract buffers from type-erased parameter
        // Pass by value so array is copied into coroutine frame
        buffer_array bufs{};
        std::size_t count = param.copy_to(bufs.data(), max_buffers);

        // Launch inner coroutine via run_async
        capy::run_async(d)(
            do_write_some(bufs, count, token, ec, bytes, h, d));
    }

    void handshake(
        std::coroutine_handle<> h,
        capy::any_executor_ref d,
        int type,
        std::stop_token token,
        system::error_code* ec) override
    {
        // Launch inner coroutine via run_async
        capy::run_async(d)(
            do_handshake(type, token, ec, h, d));
    }

    //--------------------------------------------------------------------------
    // Initialization
    //--------------------------------------------------------------------------

    system::error_code
    init_ssl()
    {
        // Create WolfSSL context for TLS client (auto-negotiate best version)
        ctx_ = wolfSSL_CTX_new(wolfTLS_client_method());
        if(!ctx_)
        {
            return system::error_code(
                wolfSSL_get_error(nullptr, 0),
                system::system_category());
        }

        // Disable certificate verification for now (TODO: make configurable)
        // This is needed when connecting without proper CA certificates
        wolfSSL_CTX_set_verify(ctx_, WOLFSSL_VERIFY_NONE, nullptr);

        // Create SSL session
        ssl_ = wolfSSL_new(ctx_);
        if(!ssl_)
        {
            int err = wolfSSL_get_error(nullptr, 0);
            wolfSSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return system::error_code(err, system::system_category());
        }

        // Set custom I/O callbacks
        wolfSSL_SSLSetIORecv(ssl_, &recv_callback);
        wolfSSL_SSLSetIOSend(ssl_, &send_callback);

        // Set this impl as the I/O context
        wolfSSL_SetIOReadCtx(ssl_, this);
        wolfSSL_SetIOWriteCtx(ssl_, this);

        // Set SNI (Server Name Indication) - required by most modern TLS servers
        // TODO: Make this configurable via API
        // WOLFSSL_SNI_HOST_NAME = 0
        wolfSSL_UseSNI(ssl_, 0, "www.boost.org", 13);

        return {};
    }
};

//------------------------------------------------------------------------------

wolfssl_stream::
wolfssl_stream(io_stream& stream)
    : io_stream(stream.context())
    , s_(stream)
{
    construct();
}

void
wolfssl_stream::
construct()
{
    auto* impl = new wolfssl_stream_impl_(s_);

    // Initialize WolfSSL
    auto ec = impl->init_ssl();
    if(ec)
    {
        delete impl;
        // For now, silently fail - could throw or store error
        return;
    }

    impl_ = impl;
}

} // namespace corosio
} // namespace boost
