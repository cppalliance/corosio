//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/capy/async_mutex.hpp>
#include <boost/capy/async_run.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/task.hpp>

// Tell WolfSSL we're using custom configuration to avoid #warning directives
// which cause hard errors on MSVC's traditional preprocessor
#ifndef WOLFSSL_CUSTOM_CONFIG
#define WOLFSSL_CUSTOM_CONFIG
#endif

#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>

#include <algorithm>
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

    Concurrency
    -----------
    async_mutex (from capy) protects underlying stream access:

      - Each SSL op acquires mutex only for underlying I/O, not entire operation
      - Separate buffers per direction to avoid conflicts
      - Intrusive queue in mutex - zero allocation, node in coroutine frame

    Key Types
    ---------
    - wolfssl_stream_impl : io_stream_impl  -- the impl stored in io_object::impl_
    - recv_callback, send_callback          -- WolfSSL I/O hooks (static)
    - do_read_some, do_write_some           -- inner coroutines with WANT_* loops
    - do_underlying_read/write              -- mutex-protected helpers
*/

namespace boost {
namespace corosio {

namespace {

// Default buffer size for TLS I/O
constexpr std::size_t default_buffer_size = 16384;

// Maximum number of buffers to handle in a single operation
constexpr std::size_t max_buffers = 8;

} // namespace

//------------------------------------------------------------------------------

struct wolfssl_stream::wolfssl_stream_impl
    : io_stream::io_stream_impl
{
    io_stream& s_;
    WOLFSSL_CTX* ctx_ = nullptr;
    WOLFSSL* ssl_ = nullptr;

    // Mutex protecting access to underlying stream
    capy::async_mutex io_mutex_;

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

    //--------------------------------------------------------------------------

    explicit
    wolfssl_stream_impl(io_stream& s)
        : s_(s)
    {
        read_in_buf_.resize(default_buffer_size);
        read_out_buf_.resize(default_buffer_size);
        write_in_buf_.resize(default_buffer_size);
        write_out_buf_.resize(default_buffer_size);
    }

    ~wolfssl_stream_impl()
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
        auto* impl = static_cast<wolfssl_stream_impl*>(ctx);
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
        auto* impl = static_cast<wolfssl_stream_impl*>(ctx);
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
    // Mutex-protected underlying I/O helpers
    //--------------------------------------------------------------------------

    /** Read from underlying stream with mutex protection.
    */
    capy::task<system::error_code>
    do_underlying_read(
        std::vector<char>& in_buf,
        std::size_t& in_pos,
        std::size_t& in_len)
    {
        // Reset buffer if empty
        if(in_pos == in_len)
        {
            in_pos = 0;
            in_len = 0;
        }

        // Acquire mutex for underlying I/O
        auto guard = co_await io_mutex_.scoped_lock();

        // Read into available space
        capy::mutable_buffer buf(
            in_buf.data() + in_len,
            in_buf.size() - in_len);

        auto [ec, n] = co_await s_.read_some(buf);
        if(ec)
            co_return ec;

        in_len += n;
        co_return system::error_code{};
    }

    /** Write to underlying stream with mutex protection.
    */
    capy::task<system::error_code>
    do_underlying_write(
        std::vector<char>& out_buf,
        std::size_t& out_len)
    {
        // Acquire mutex for underlying I/O
        auto guard = co_await io_mutex_.scoped_lock();

        while(out_len > 0)
        {
            capy::mutable_buffer buf(out_buf.data(), out_len);
            auto [ec, n] = co_await s_.write_some(buf);
            if(ec)
                co_return ec;

            // Remove written data from buffer
            if(n < out_len)
            {
                std::memmove(out_buf.data(),
                    out_buf.data() + n, out_len - n);
            }
            out_len -= n;
        }
        co_return system::error_code{};
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
        capy::mutable_buffer* dest_bufs,
        std::size_t buf_count,
        std::stop_token token,
        system::error_code* ec_out,
        std::size_t* bytes_out,
        std::coroutine_handle<> continuation,
        capy::any_dispatcher d)
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
                        // Need to read from underlying stream
                        ec = co_await do_underlying_read(
                            read_in_buf_, read_in_pos_, read_in_len_);
                        if(ec)
                            goto done;
                    }
                    else if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Need to flush output (can happen during renegotiation)
                        ec = co_await do_underlying_write(
                            read_out_buf_, read_out_len_);
                        if(ec)
                            goto done;
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

        // Resume the original caller via dispatcher
        d(capy::coro{continuation}).resume();
        co_return;
    }

    /** Inner coroutine that performs TLS write with WANT_WRITE loop.

        Calls wolfSSL_write in a loop, performing async writes to the
        underlying stream when needed.
    */
    capy::task<>
    do_write_some(
        capy::mutable_buffer* src_bufs,
        std::size_t buf_count,
        std::stop_token token,
        system::error_code* ec_out,
        std::size_t* bytes_out,
        std::coroutine_handle<> continuation,
        capy::any_dispatcher d)
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
                        if(write_out_len_ > 0)
                        {
                            ec = co_await do_underlying_write(
                                write_out_buf_, write_out_len_);
                            if(ec)
                                goto done;
                        }
                        goto done;
                    }
                }
                else
                {
                    int err = wolfSSL_get_error(ssl_, ret);

                    if(err == WOLFSSL_ERROR_WANT_WRITE)
                    {
                        // Need to flush output buffer
                        ec = co_await do_underlying_write(
                            write_out_buf_, write_out_len_);
                        if(ec)
                            goto done;
                    }
                    else if(err == WOLFSSL_ERROR_WANT_READ)
                    {
                        // Need to read (can happen during renegotiation)
                        ec = co_await do_underlying_read(
                            write_in_buf_, write_in_pos_, write_in_len_);
                        if(ec)
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
        *bytes_out = total_written;

        // Resume the original caller via dispatcher
        d(capy::coro{continuation}).resume();
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
        capy::any_dispatcher d,
        any_bufref& param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        // Extract buffers from type-erased parameter
        capy::mutable_buffer bufs[max_buffers];
        std::size_t count = param.copy_to(bufs, max_buffers);

        // Launch inner coroutine via async_run
        capy::async_run(d)(
            do_read_some(bufs, count, token, ec, bytes, h, d));
    }

    void write_some(
        std::coroutine_handle<> h,
        capy::any_dispatcher d,
        any_bufref& param,
        std::stop_token token,
        system::error_code* ec,
        std::size_t* bytes) override
    {
        // Extract buffers from type-erased parameter
        capy::mutable_buffer bufs[max_buffers];
        std::size_t count = param.copy_to(bufs, max_buffers);

        // Launch inner coroutine via async_run
        capy::async_run(d)(
            do_write_some(bufs, count, token, ec, bytes, h, d));
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

        return {};
    }
};

//------------------------------------------------------------------------------

void
wolfssl_stream::
construct()
{
    auto* impl = new wolfssl_stream_impl(*s_);

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
