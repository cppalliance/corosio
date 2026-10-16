//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
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
#include <boost/capy/results.hpp>
#include <boost/capy/task.hpp>

#include <concepts>
#include <memory>
#include <system_error>
#include <tuple>

namespace boost::corosio {

/** Encrypts and decrypts a stream using WolfSSL.

    This class wraps an underlying stream satisfying `capy::Stream`
    and provides TLS encryption using the WolfSSL library.

    Derives from @ref tls_stream to provide a runtime-polymorphic
    interface. The TLS operations are implemented as coroutines
    that orchestrate reads and writes on the underlying stream.

    @par Construction Modes

    Two construction modes are supported:

    - **Owning**: Pass stream by value. The `wolfssl_stream` takes
      ownership. The stream is moved into internal storage.

    - **Reference**: Pass stream by pointer. The `wolfssl_stream`
      does not own the stream. The caller must ensure the stream
      outlives this object.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe, with one exception: one read operation and
    one write operation may be in flight simultaneously. `shutdown()`
    may overlap a pending read. On a multi-threaded execution context,
    all operations on one stream must run within the same
    `capy::strand`, or must otherwise never run concurrently. A
    single-threaded context needs no strand.

    @par Example
    @par !example wolfssl_stream

    @see tls_stream, openssl_stream
*/
class BOOST_COROSIO_DECL wolfssl_stream final : public tls_stream
{
    struct implementation;
    implementation* impl_;

public:
    /** Construct a WolfSSL stream (owning mode).

        Takes ownership of the underlying stream by moving it into
        internal storage. The stream is destroyed when this
        `wolfssl_stream` is destroyed.

        @param stream The stream to take ownership of. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
        requires(!std::same_as<std::decay_t<S>, wolfssl_stream>)
    wolfssl_stream(S stream, tls_context const& ctx)
        : impl_(adopt(std::make_unique<S>(std::move(stream)), ctx))
    {
    }

    /** Construct a WolfSSL stream (reference mode).

        Wraps the underlying stream without taking ownership. The
        caller must ensure the stream remains valid for the lifetime
        of this `wolfssl_stream`.

        @param stream Pointer to the stream to wrap. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
    wolfssl_stream(S* stream, tls_context const& ctx)
        : impl_(make_implementation(capy::any_stream(stream), {}, ctx))
    {
    }

    /** Destroy the WolfSSL stream.

        Operations still in flight complete with
        `capy::error::canceled` without touching this object, and the
        TLS state they need is released when the last of them
        finishes. In owning mode the underlying stream is destroyed
        when the last of those operations finishes. An underlying
        stream with a `cancel()` member is told to cancel them now. In reference
        mode, and for an owned stream without `cancel()`, an operation
        parked on the underlying stream finishes when that stream's
        operation does.
    */
    ~wolfssl_stream() override;

    /** Move construct from another WolfSSL stream.

        Operations in flight on @p other continue on this stream.

        @param other The source stream. After the move,
            @p other may only be destroyed or assigned to.
    */
    wolfssl_stream(wolfssl_stream&& other) noexcept;

    /** Move assign from another WolfSSL stream.

        Operations in flight on this stream complete as if it were
        destroyed; those on @p other continue on this stream.

        @param other The source stream. After the move,
            @p other may only be destroyed or assigned to.

        @return `*this`.
    */
    wolfssl_stream& operator=(wolfssl_stream&& other) noexcept;

    /** Asynchronously perform the TLS handshake.

        Suspends the calling coroutine until the handshake
        completes, an error occurs, or the operation is
        cancelled via stop token.

        @pre The underlying stream must be connected. No other
            TLS operation may be in progress on this stream.

        @param role The handshake role, client or server.

        @return An awaitable yielding `(error_code)`.
    */
    [[nodiscard]] capy::task<capy::results<>> handshake(tls_role role) override;

    /** Asynchronously shut down the TLS session.

        Sends a close_notify alert and waits for the peer's
        close_notify response. Supports cancellation via
        stop token.

        @pre A handshake must have completed successfully. May overlap
            a pending read. That read completes with `capy::error::eof`
            when the peer answers the close_notify. No concurrent write
            may be in progress.

        @par Postconditions
        If the transport ends before the peer's close_notify is
        received, the result is `capy::error::stream_truncated`, not
        success. A shutdown stopped mid-flight reports canceled. Any
        other transport error propagates unchanged.

        @return An awaitable yielding `(error_code)`.
    */
    [[nodiscard]] capy::task<capy::results<>> shutdown() override;

    /// Cancel operations pending on the owned underlying stream.
    void cancel() noexcept override;

    /** Reset TLS session state for reuse.

        Clears internal buffers and session data so the stream
        can perform a new handshake on the same underlying
        connection. The previous session is discarded and never
        resumed, so a handshake after `reset()` is always a full
        handshake.

        @pre No TLS operation may be in progress on this stream.
    */
    void reset() override;

    /** Set the peer hostname for SNI and certificate verification.

        @param hostname The peer name to send as SNI and match against the
            certificate.
    */
    void set_hostname(std::string_view hostname) override;

    /// Return the underlying stream.
    capy::any_stream& next_layer() noexcept override;

    /// Return the underlying stream.
    capy::any_stream const& next_layer() const noexcept override;

    /// Return the TLS backend name ("wolfssl").
    std::string_view name() const noexcept override;

    /// Return the ALPN protocol negotiated during the handshake, or empty.
    std::string_view alpn_protocol() const noexcept override;

protected:
    /// @copydoc tls_stream::do_read_some
    capy::task<capy::results<std::size_t>> do_read_some(
        capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers)
        override;

    /// @copydoc tls_stream::do_write_some
    capy::task<capy::results<std::size_t>> do_write_some(
        capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers)
        override;

private:
    static implementation* make_implementation(
        capy::any_stream stream,
        detail::tls_owned_transport owned,
        tls_context const& ctx);

    // The transport is boxed apart from the any_stream that refers to
    // it, so it outlives any operation still parked on it after the
    // stream is destroyed.
    template<class S>
    static implementation*
    adopt(std::unique_ptr<S> owned, tls_context const& ctx)
    {
        auto* impl = make_implementation(
            capy::any_stream(owned.get()),
            detail::make_owned_transport(owned.get()), ctx);
        std::ignore = owned.release();
        return impl;
    }
};

/** Return the error category for raw WolfSSL errors.

    Errors reported by @ref wolfssl_stream that originate from
    `wolfSSL_get_error` are assigned this category. Its `message()`
    decodes the WolfSSL error code using WolfSSL's own diagnostic
    strings. Printing such an `error_code` therefore yields a readable
    description, for example "ASN no signer error to confirm failure".

    @return A reference to a static category object with name
        `"corosio.wolfssl"`.
*/
BOOST_COROSIO_DECL std::error_category const& wolfssl_category() noexcept;

/** Report whether this build's WolfSSL can honor a verify callback.

    A verify callback installed via @ref tls_context::set_verify_callback
    can only be honored on a successful handshake when the linked WolfSSL
    was built with `WOLFSSL_ALWAYS_VERIFY_CB` (implied by
    `--enable-opensslextra`). On a build without it, WolfSSL invokes the
    callback only on verification failure, so a callback that tightens
    verification would silently fail open. The @ref wolfssl_stream backend
    instead fails the handshake with `std::errc::function_not_supported`
    when a callback is present.

    This function lets callers detect that situation up front.

    @return `true` if verify callbacks are fully supported by this build,
        `false` if installing one causes the handshake to fail.

    @see tls_context::set_verify_callback
*/
BOOST_COROSIO_DECL bool wolfssl_supports_verify_callback() noexcept;

/** Report whether this WolfSSL build can negotiate ALPN.

    ALPN requires a WolfSSL built with `HAVE_ALPN`. On a build without
    it, offering protocols via @ref tls_context::set_alpn fails the
    handshake with `std::errc::function_not_supported` rather than
    silently negotiating nothing.

    @return `true` if ALPN is supported by this build, `false` if
        offering protocols causes the handshake to fail.

    @see tls_context::set_alpn, tls_stream::alpn_protocol
*/
BOOST_COROSIO_DECL bool wolfssl_supports_alpn() noexcept;

/** Report whether this WolfSSL build can check certificate revocation.

    CRL checking requires a WolfSSL built with `HAVE_CRL`. On a build
    without it, supplying a CRL or a revocation policy fails the handshake
    with `std::errc::function_not_supported` rather than silently skipping
    revocation.

    @return `true` if revocation checking is supported by this build.

    @see tls_context::add_crl, tls_context::set_revocation_policy
*/
BOOST_COROSIO_DECL bool wolfssl_supports_crl() noexcept;

/** Report whether this WolfSSL build can verify IP-literal hostnames.

    Matching an IP literal against a certificate's iPAddress entries
    requires a WolfSSL built with both `OPENSSL_EXTRA` and
    `WOLFSSL_IP_ALT_NAME`. The first routes the address into the verify
    parameters the certificate check consults. The second records
    iPAddress entries during parsing. On a build lacking either,
    `wolfSSL_check_ip_address` reports success. Verification silently
    checks nothing. A handshake with an IP literal set via @ref
    tls_stream::set_hostname therefore fails with
    `std::errc::function_not_supported` rather than proceed unverified.

    @return `true` if IP-literal verification is supported by this build.

    @see tls_stream::set_hostname
*/
BOOST_COROSIO_DECL bool wolfssl_supports_ip_alt_name() noexcept;

} // namespace boost::corosio

#endif
