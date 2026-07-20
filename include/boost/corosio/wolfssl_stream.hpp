//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
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
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/concept/stream.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>

#include <concepts>
#include <system_error>

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
    ctx.set_verify_mode(tls_verify_mode::peer);

    corosio::tcp_socket sock(ioc);
    co_await sock.connect(endpoint);

    // Reference mode - sock must outlive tls
    corosio::wolfssl_stream tls(&sock, ctx);
    tls.set_hostname("example.com");
    auto [ec] = co_await tls.handshake(wolfssl_stream::client);

    // Or owning mode - tls owns the socket
    corosio::wolfssl_stream tls2(std::move(sock), ctx);
    @endcode

    @see tls_stream, openssl_stream
*/
class BOOST_COROSIO_DECL wolfssl_stream final : public tls_stream
{
    struct impl;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // capy::any_stream, dll-interface
    capy::any_stream stream_; // must be first - impl_ holds reference
    BOOST_COROSIO_MSVC_WARNING_POP
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
    wolfssl_stream(S stream, tls_context const& ctx)
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
    wolfssl_stream(S* stream, tls_context const& ctx)
        : stream_(stream)
        , impl_(make_impl(stream_, ctx))
    {
    }

    /** Destructor.

        Releases the underlying WolfSSL resources. If constructed
        in owning mode, also destroys the underlying stream.
    */
    ~wolfssl_stream() override;

    /** Move construct from another WolfSSL stream.

        @param other The source stream. After the move,
            @p other is in a valid but unspecified state.
    */
    wolfssl_stream(wolfssl_stream&& other) noexcept;

    /** Move assign from another WolfSSL stream.

        @param other The source stream. After the move,
            @p other is in a valid but unspecified state.

        @return `*this`.
    */
    wolfssl_stream& operator=(wolfssl_stream&& other) noexcept;

    /** Perform the TLS handshake asynchronously.

        Suspends the calling coroutine until the handshake
        completes, an error occurs, or the operation is
        cancelled via stop token.

        @par Preconditions
        The underlying stream must be connected. No other
        TLS operation may be in progress on this stream.

        @param type The handshake role (client or server).

        @return An awaitable yielding `(error_code)`.
    */
    capy::io_task<> handshake(handshake_type type) override;

    /** Shut down the TLS session asynchronously.

        Sends a close_notify alert and waits for the peer's
        close_notify response. Supports cancellation via
        stop token.

        @par Preconditions
        A handshake must have completed successfully. No
        other TLS operation may be in progress on this stream.

        @return An awaitable yielding `(error_code)`.
    */
    capy::io_task<> shutdown() override;

    /** Reset TLS session state for reuse.

        Clears internal buffers and session data so the stream
        can perform a new handshake on the same underlying
        connection.

        @par Preconditions
        No TLS operation may be in progress on this stream.
    */
    void reset() override;

    /// Set the peer hostname for SNI and certificate verification.
    void set_hostname(std::string_view hostname) override;

    /// Return the underlying stream.
    capy::any_stream& next_layer() noexcept override
    {
        return stream_;
    }

    /// Return the underlying stream.
    capy::any_stream const& next_layer() const noexcept override
    {
        return stream_;
    }

    /// Return the TLS backend name ("wolfssl").
    std::string_view name() const noexcept override;

    /// Return the ALPN protocol negotiated during the handshake, or empty.
    std::string_view alpn_protocol() const noexcept override;

protected:
    capy::io_task<std::size_t> do_read_some(
        capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers) override;

    capy::io_task<std::size_t> do_write_some(
        capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers) override;

private:
    static impl* make_impl(capy::any_stream& stream, tls_context const& ctx);
};

/** Return the error category for raw WolfSSL errors.

    Errors reported by @ref wolfssl_stream that originate from
    `wolfSSL_get_error` are assigned this category. Its `message()`
    decodes the WolfSSL error code using WolfSSL's own diagnostic
    strings, so printing such an `error_code` yields a readable
    description (for example, "ASN no signer error to confirm failure").

    @return A reference to a static category object with name
        `"corosio.wolfssl"`.
*/
BOOST_COROSIO_DECL std::error_category const&
wolfssl_category() noexcept;

/** Report whether this build's WolfSSL can honor a verify callback.

    A verify callback installed via @ref tls_context::set_verify_callback
    can only be honored on a successful handshake when the linked WolfSSL
    was built with `WOLFSSL_ALWAYS_VERIFY_CB` (implied by
    `--enable-opensslextra`). On a build without it, WolfSSL invokes the
    callback only on verification failure, so a callback that tightens
    verification would silently fail open; the @ref wolfssl_stream backend
    instead fails the handshake with `std::errc::function_not_supported`
    when a callback is present.

    This function lets callers detect that situation up front.

    @return `true` if verify callbacks are fully supported by this build,
        `false` if installing one will cause the handshake to fail.

    @see tls_context::set_verify_callback
*/
BOOST_COROSIO_DECL bool
wolfssl_supports_verify_callback() noexcept;

/** Report whether this WolfSSL build can negotiate ALPN.

    ALPN requires a WolfSSL built with `HAVE_ALPN`. On a build without
    it, offering protocols via @ref tls_context::set_alpn fails the
    handshake with `std::errc::function_not_supported` rather than
    silently negotiating nothing.

    @return `true` if ALPN is supported by this build, `false` if
        offering protocols will cause the handshake to fail.

    @see tls_context::set_alpn, tls_stream::alpn_protocol
*/
BOOST_COROSIO_DECL bool
wolfssl_supports_alpn() noexcept;

/** Report whether this WolfSSL build can check certificate revocation.

    CRL checking requires a WolfSSL built with `HAVE_CRL`. On a build
    without it, supplying a CRL or a revocation policy fails the handshake
    with `std::errc::function_not_supported` rather than silently skipping
    revocation.

    @return `true` if revocation checking is supported by this build.

    @see tls_context::add_crl, tls_context::set_revocation_policy
*/
BOOST_COROSIO_DECL bool
wolfssl_supports_crl() noexcept;

} // namespace boost::corosio

#endif
