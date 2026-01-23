//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_TLS_STREAM_HPP
#define BOOST_COROSIO_TLS_TLS_STREAM_HPP

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <coroutine>
#include <stop_token>

namespace boost {
namespace corosio {

/** Abstract base class for TLS streams.

    This class provides the common interface for TLS stream implementations.
    It derives from @ref io_stream to inherit read and write operations,
    and adds the TLS-specific handshake and shutdown operations.

    Concrete implementations (e.g., wolfssl_stream, openssl_stream) derive
    from this class and provide backend-specific functionality.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.
*/
class BOOST_COROSIO_DECL tls_stream : public io_stream
{
    struct handshake_awaitable
    {
        tls_stream& stream_;
        int type_;
        std::stop_token token_;
        mutable system::error_code ec_;

        handshake_awaitable(
            tls_stream& stream,
            int type) noexcept
            : stream_(stream)
            , type_(type)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if(token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled)};
            return {ec_};
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex) -> std::coroutine_handle<>
        {
            stream_.get().handshake(h, ex, type_, token_, &ec_);
            return std::noop_coroutine();
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            stream_.get().handshake(h, ex, type_, token_, &ec_);
            return std::noop_coroutine();
        }
    };

    struct shutdown_awaitable
    {
        tls_stream& stream_;
        std::stop_token token_;
        mutable system::error_code ec_;

        explicit
        shutdown_awaitable(tls_stream& stream) noexcept
            : stream_(stream)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<> await_resume() const noexcept
        {
            if(token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled)};
            return {ec_};
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex) -> std::coroutine_handle<>
        {
            stream_.get().shutdown(h, ex, token_, &ec_);
            return std::noop_coroutine();
        }

        template<typename Ex>
        auto await_suspend(
            std::coroutine_handle<> h,
            Ex const& ex,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            stream_.get().shutdown(h, ex, token_, &ec_);
            return std::noop_coroutine();
        }
    };

public:
    /** Different handshake types. */
    enum handshake_type
    {
        /** Perform handshaking as a client. */
        client,

        /** Perform handshaking as a server. */
        server
    };

    /** Perform the TLS handshake asynchronously.

        This function initiates the TLS handshake process. For client
        connections, this sends the ClientHello and processes the
        server's response. For server connections, this waits for the
        ClientHello and sends the server's response.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param type The type of handshaking to perform (client or server).

        @return An awaitable that completes with `io_result<>`.
            Returns success on successful handshake, or an error code
            on failure including:
            - SSL/TLS errors from the underlying library
            - operation_canceled: Cancelled via stop_token

        @par Preconditions
        The underlying stream must be connected.

        @par Example
        @code
        // Client handshake with error code
        auto [ec] = co_await secure.handshake(tls_stream::client);
        if(ec) { ... }

        // Or with exceptions
        (co_await secure.handshake(tls_stream::client)).value();
        @endcode
    */
    auto handshake(handshake_type type)
    {
        return handshake_awaitable(*this, type);
    }

    /** Perform a graceful TLS shutdown asynchronously.

        This function initiates the TLS shutdown sequence by sending a
        close_notify alert and waiting for the peer's close_notify response.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @return An awaitable that completes with `io_result<>`.
            Returns success on successful shutdown, or an error code
            on failure.

        @par Preconditions
        There must be no pending read or write operations on this stream.
        The application must ensure all read_some and write_some operations
        have completed before calling shutdown.

        @par Example
        @code
        auto [ec] = co_await secure.shutdown();
        if(ec) { ... }
        @endcode
    */
    auto shutdown()
    {
        return shutdown_awaitable(*this);
    }

    /** Returns a reference to the underlying stream.

        @return Reference to the wrapped io_stream.
    */
    io_stream& next_layer() noexcept
    {
        return s_;
    }

    /** Returns a const reference to the underlying stream.

        @return Const reference to the wrapped io_stream.
    */
    io_stream const& next_layer() const noexcept
    {
        return s_;
    }

    struct tls_stream_impl : io_stream_impl
    {
        virtual void handshake(
            std::coroutine_handle<>,
            capy::executor_ref,
            int,
            std::stop_token,
            system::error_code*) = 0;

        virtual void shutdown(
            std::coroutine_handle<>,
            capy::executor_ref,
            std::stop_token,
            system::error_code*) = 0;
    };

protected:
    explicit
    tls_stream(io_stream& stream) noexcept
        : io_stream(stream.context())
        , s_(stream)
    {
    }

    io_stream& s_;

private:
    tls_stream_impl& get() const noexcept
    {
        return *static_cast<tls_stream_impl*>(impl_);
    }
};

} // namespace corosio
} // namespace boost

#endif
#endif
