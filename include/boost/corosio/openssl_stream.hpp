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

#ifndef BOOST_COROSIO_OPENSSL_STREAM_HPP
#define BOOST_COROSIO_OPENSSL_STREAM_HPP

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

/** Encrypts and decrypts a stream using OpenSSL.

    This class wraps an underlying stream satisfying `capy::Stream`
    and provides TLS encryption using the OpenSSL library.

    Derives from @ref tls_stream to provide a runtime-polymorphic
    interface. The TLS operations are implemented as coroutines
    that orchestrate reads and writes on the underlying stream.

    @par Construction Modes

    Two construction modes are supported:

    - **Owning**: Pass stream by value. The `openssl_stream` takes
      ownership. The stream is moved into internal storage.

    - **Reference**: Pass stream by pointer. The `openssl_stream`
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
    @par !example openssl_stream

    @see @ref tls_stream, @ref wolfssl_stream
*/
class BOOST_COROSIO_DECL openssl_stream final : public tls_stream
{
    struct implementation;
    implementation* impl_;

public:
    /** Construct an OpenSSL stream (owning mode).

        Takes ownership of the underlying stream by moving it into
        internal storage. The stream is destroyed when this
        `openssl_stream` is destroyed.

        @param stream The stream to take ownership of. Must satisfy
            `capy::Stream` and must not be an `openssl_stream`; that
            case binds to the move constructor instead.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
        requires(!std::same_as<std::decay_t<S>, openssl_stream>)
    openssl_stream(S stream, tls_context const& ctx)
        : impl_(adopt(std::make_unique<S>(std::move(stream)), ctx))
    {
    }

    /** Construct an OpenSSL stream (reference mode).

        Wraps the underlying stream without taking ownership. The
        caller must ensure the stream remains valid for the lifetime
        of this `openssl_stream`.

        @param stream Pointer to the stream to wrap. Must satisfy
            `capy::Stream`.
        @param ctx The TLS context containing configuration.
    */
    template<capy::Stream S>
    openssl_stream(S* stream, tls_context const& ctx)
        : impl_(make_implementation(capy::any_stream(stream), {}, ctx))
    {
    }

    /** Destroy the OpenSSL stream.

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
    ~openssl_stream() override;

    /** Move construct from another OpenSSL stream.

        Operations in flight on @p other continue on this stream.

        @param other The source stream. After the move,
            @p other may only be destroyed or assigned to.
    */
    openssl_stream(openssl_stream&& other) noexcept;

    /** Move assign from another OpenSSL stream.

        Operations in flight on this stream complete as if it were
        destroyed; those on @p other continue on this stream.

        @param other The source stream. After the move,
            @p other may only be destroyed or assigned to.

        @return `*this`.
    */
    openssl_stream& operator=(openssl_stream&& other) noexcept;

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

        @note If the backend cannot restore a clean session state,
        subsequent handshakes fail rather than proceed on a
        partially cleared session.
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

    /// Return the TLS backend name ("openssl").
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

/** Return the error category for raw OpenSSL errors.

    Errors reported by @ref openssl_stream that originate from the OpenSSL
    error queue (`ERR_get_error`) are assigned this category. Its
    `message()` decodes the packed OpenSSL error code using OpenSSL's own
    diagnostic strings. Printing such an `error_code` therefore yields a
    readable description, for example "certificate verify failed".

    OpenSSL errors whose library is `ERR_LIB_SYS` are reported with
    `std::system_category()` instead, since their reason code is a genuine
    `errno` value.

    @return A reference to a static category object with name
        `"corosio.openssl"`.
*/
BOOST_COROSIO_DECL std::error_category const& openssl_category() noexcept;

} // namespace boost::corosio

#endif
