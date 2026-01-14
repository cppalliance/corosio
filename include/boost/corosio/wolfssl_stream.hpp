//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WOLFSSL_STREAM_HPP
#define BOOST_COROSIO_WOLFSSL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_result.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/capy/ex/any_dispatcher.hpp>

#include <coroutine>
#include <stop_token>

namespace boost {
namespace corosio {

/** A TLS stream using WolfSSL.

    This class wraps an underlying stream derived from @ref io_stream
    and provides TLS encryption using the WolfSSL library.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    corosio::socket raw_socket(ioc);
    raw_socket.open();
    co_await raw_socket.connect(endpoint);

    corosio::wolfssl_stream secure(raw_socket);
    co_await secure.handshake(wolfssl_stream::client);
    // Use secure stream for TLS communication
    @endcode
*/
class BOOST_COROSIO_DECL wolfssl_stream : public io_stream
{
    struct handshake_awaitable
    {
        wolfssl_stream& stream_;
        int type_;
        std::stop_token token_;
        mutable system::error_code ec_;

        handshake_awaitable(
            wolfssl_stream& stream,
            int type) noexcept
            : stream_(stream)
            , type_(type)
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        io_result<> await_resume() const noexcept
        {
            if(token_.stop_requested())
                return {make_error_code(system::errc::operation_canceled)};
            return {ec_};
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d) -> std::coroutine_handle<>
        {
            stream_.get().handshake(h, d, type_, token_, &ec_);
            return std::noop_coroutine();
        }

        template<capy::dispatcher Dispatcher>
        auto await_suspend(
            std::coroutine_handle<> h,
            Dispatcher const& d,
            std::stop_token token) -> std::coroutine_handle<>
        {
            token_ = std::move(token);
            stream_.get().handshake(h, d, type_, token_, &ec_);
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

    /** Construct a WolfSSL stream wrapping an existing stream.

        The underlying stream must remain valid for the lifetime of
        this wolfssl_stream object.

        @param stream Reference to the underlying stream to wrap.
    */
    explicit
    wolfssl_stream(io_stream& stream);

    /** Perform the TLS handshake asynchronously.

        This function initiates the TLS handshake process. For client
        connections, this calls `wolfSSL_connect()`. For server
        connections, this calls `wolfSSL_accept()`.

        The operation supports cancellation via `std::stop_token` through
        the affine awaitable protocol. If the associated stop token is
        triggered, the operation completes immediately with
        `errc::operation_canceled`.

        @param type The type of handshaking to perform (client or server).

        @return An awaitable that completes with `io_result<>`.
            Returns success on successful handshake, or an error code
            on failure including:
            - SSL/TLS errors from WolfSSL
            - operation_canceled: Cancelled via stop_token

        @par Preconditions
        The underlying stream must be connected.

        @par Example
        @code
        // Client handshake with error code
        auto [ec] = co_await secure.handshake(wolfssl_stream::client);
        if(ec) { ... }

        // Or with exceptions
        (co_await secure.handshake(wolfssl_stream::client)).value();
        @endcode
    */
    auto handshake(handshake_type type)
    {
        return handshake_awaitable(*this, type);
    }

    struct wolfssl_stream_impl : io_stream_impl
    {
        virtual void handshake(
            std::coroutine_handle<>,
            capy::any_dispatcher,
            int,
            std::stop_token,
            system::error_code*) = 0;
    };

private:
    void construct();

    wolfssl_stream_impl& get() const noexcept
    {
        return *static_cast<wolfssl_stream_impl*>(impl_);
    }

    io_stream& s_;
};

} // namespace corosio
} // namespace boost

#endif
