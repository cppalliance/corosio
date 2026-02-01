//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_OPENSSL_STREAM_HPP
#define BOOST_COROSIO_TLS_OPENSSL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tls/context.hpp>
#include <boost/corosio/tls/tls_stream.hpp>
#include <boost/capy/concept/stream.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>

#include <concepts>

namespace boost::corosio {

/** A TLS stream using OpenSSL.

    This class wraps an underlying stream satisfying `capy::Stream`
    and provides TLS encryption using the OpenSSL library.

    Derives from @ref tls_stream to provide a runtime-polymorphic
    interface. The TLS operations are implemented as coroutines
    that orchestrate reads and writes on the underlying stream.

    @par Construction Modes

    Two construction modes are supported:

    - **Owning**: Pass stream by value. The openssl_stream takes
      ownership and the stream is moved into internal storage.

    - **Reference**: Pass stream by pointer. The openssl_stream
      does not own the stream; the caller must ensure the stream
      outlives this object.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    tls::context ctx;
    ctx.set_hostname("example.com");
    ctx.set_verify_mode(tls::verify_mode::peer);

    corosio::tcp_socket sock(ioc);
    co_await sock.connect(endpoint);

    // Reference mode - sock must outlive tls
    corosio::openssl_stream tls(&sock, ctx);
    auto [ec] = co_await tls.handshake(openssl_stream::client);

    // Or owning mode - tls owns the socket
    corosio::openssl_stream tls2(std::move(sock), ctx);
    @endcode

    @see tls_stream, wolfssl_stream
*/
class BOOST_COROSIO_DECL openssl_stream final
    : public tls_stream
{
    struct impl;
    capy::any_stream stream_;  // must be first - impl_ holds reference
    impl* impl_;

public:
    /** Construct an OpenSSL stream (owning mode).

        Takes ownership of the underlying stream by moving it into
        internal storage. The stream will be destroyed when this
        openssl_stream is destroyed.

        @param stream The stream to take ownership of. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
        requires (!std::same_as<std::decay_t<S>, openssl_stream>)
    openssl_stream(S stream, tls::context ctx)
        : stream_(std::move(stream))
        , impl_(make_impl(stream_, ctx))
    {
    }

    /** Construct an OpenSSL stream (reference mode).

        Wraps the underlying stream without taking ownership. The
        caller must ensure the stream remains valid for the lifetime
        of this openssl_stream.

        @param stream Pointer to the stream to wrap. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
    openssl_stream(S* stream, tls::context ctx)
        : stream_(stream)
        , impl_(make_impl(stream_, ctx))
    {
    }

    /** Destructor.

        Releases the underlying OpenSSL resources. If constructed
        in owning mode, also destroys the underlying stream.
    */
    ~openssl_stream();

    openssl_stream(openssl_stream&&) noexcept;
    openssl_stream& operator=(openssl_stream&&) noexcept;

    capy::io_task<>
    handshake(handshake_type type) override;

    capy::io_task<>
    shutdown() override;

    capy::any_stream&
    next_layer() noexcept override
    {
        return stream_;
    }

    capy::any_stream const&
    next_layer() const noexcept override
    {
        return stream_;
    }

protected:
    capy::io_task<std::size_t>
    do_read_some(io_buffer_param buffers) override;

    capy::io_task<std::size_t>
    do_write_some(io_buffer_param buffers) override;

private:
    static impl*
    make_impl(capy::any_stream& stream, tls::context const& ctx);
};

} // namespace boost::corosio

#endif
