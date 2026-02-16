//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WOLFSSL_STREAM_HPP
#define BOOST_COROSIO_WOLFSSL_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/capy/buffers/buffer_array.hpp>
#include <boost/capy/concept/stream.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>

#include <concepts>

namespace boost::corosio {

/** A TLS stream using WolfSSL.

    This class wraps an underlying stream satisfying `capy::Stream`
    and provides TLS encryption using the WolfSSL library.

    Derives from @ref tls_stream to provide a runtime-polymorphic
    interface. The TLS operations are implemented as coroutines
    that orchestrate reads and writes on the underlying stream.

    @par Construction Modes

    Two construction modes are supported:

    - **Owning**: Pass stream by value. The wolfssl_stream takes
      ownership and the stream is moved into internal storage.

    - **Reference**: Pass stream by pointer. The wolfssl_stream
      does not own the stream; the caller must ensure the stream
      outlives this object.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    tls_context ctx;
    ctx.set_hostname("example.com");
    ctx.set_verify_mode(tls_verify_mode::peer);

    corosio::tcp_socket sock(ioc);
    co_await sock.connect(endpoint);

    // Reference mode - sock must outlive tls
    corosio::wolfssl_stream tls(&sock, ctx);
    auto [ec] = co_await tls.handshake(wolfssl_stream::client);

    // Or owning mode - tls owns the socket
    corosio::wolfssl_stream tls2(std::move(sock), ctx);
    @endcode

    @see tls_stream, openssl_stream
*/
class BOOST_COROSIO_DECL wolfssl_stream final : public tls_stream
{
    struct impl;
    capy::any_stream stream_; // must be first - impl_ holds reference
    impl* impl_;

public:
    /** Construct a WolfSSL stream (owning mode).

        Takes ownership of the underlying stream by moving it into
        internal storage. The stream will be destroyed when this
        wolfssl_stream is destroyed.

        @param stream The stream to take ownership of. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
        requires(!std::same_as<std::decay_t<S>, wolfssl_stream>)
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    wolfssl_stream(S stream, tls_context ctx)
        : stream_(std::move(stream))
        , impl_(make_impl(stream_, ctx))
    {
    }

    /** Construct a WolfSSL stream (reference mode).

        Wraps the underlying stream without taking ownership. The
        caller must ensure the stream remains valid for the lifetime
        of this wolfssl_stream.

        @param stream Pointer to the stream to wrap. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    wolfssl_stream(S* stream, tls_context ctx)
        : stream_(stream)
        , impl_(make_impl(stream_, ctx))
    {
    }

    /** Destructor.

        Releases the underlying WolfSSL resources. If constructed
        in owning mode, also destroys the underlying stream.
    */
    ~wolfssl_stream() override;

    wolfssl_stream(wolfssl_stream&&) noexcept;
    wolfssl_stream& operator=(wolfssl_stream&&) noexcept;

    capy::io_task<> handshake(handshake_type type) override;

    capy::io_task<> shutdown() override;

    void reset() override;

    capy::any_stream& next_layer() noexcept override
    {
        return stream_;
    }

    capy::any_stream const& next_layer() const noexcept override
    {
        return stream_;
    }

    std::string_view name() const noexcept override;

protected:
    capy::io_task<std::size_t> do_read_some(
        capy::mutable_buffer_array<capy::detail::max_iovec_> buffers) override;

    capy::io_task<std::size_t> do_write_some(
        capy::const_buffer_array<capy::detail::max_iovec_> buffers) override;

private:
    static impl* make_impl(capy::any_stream& stream, tls_context const& ctx);
};

} // namespace boost::corosio

#endif
