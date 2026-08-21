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

#ifndef BOOST_COROSIO_TLS_STREAM_HPP
#define BOOST_COROSIO_TLS_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/detail/buffer_array.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>

#include <cstddef>
#include <string_view>

namespace boost::corosio {

/** TLS handshake role.

    Specifies whether to perform the TLS handshake as a client or server.

    @see tls_stream::handshake
*/
enum class tls_role
{
    /// Perform handshake as the connecting client.
    client,

    /// Perform handshake as the accepting server.
    server
};

/** Abstract base class for TLS streams.

    This class provides a runtime-polymorphic interface for TLS
    implementations. Derived classes (openssl_stream, wolfssl_stream)
    implement the virtual functions to provide backend-specific
    TLS functionality.

    Unlike @ref io_stream which represents OS-level I/O completed
    by the kernel, TLS streams are coroutine-based: their operations
    are implemented as coroutines that orchestrate sub-operations
    on the underlying stream.

    The non-virtual template wrappers (`read_some`, `write_some`)
    satisfy the `capy::Stream` concept, enabling TLS streams to
    be used anywhere a Stream is expected.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe, with one exception: one read operation and
    one write operation may be in flight simultaneously. `shutdown()`
    may overlap a pending read. When the execution context runs on
    multiple threads, all operations on one stream must be performed
    within the same `capy::strand` (or otherwise never run
    concurrently); a single-threaded context needs no strand.

    @see openssl_stream, wolfssl_stream
*/
class BOOST_COROSIO_DECL tls_stream
{
public:
    /// Destroy the TLS stream.
    virtual ~tls_stream() = default;

    tls_stream(tls_stream const&)            = delete;
    tls_stream& operator=(tls_stream const&) = delete;

    /** Initiate an asynchronous read operation.

        Reads decrypted data into the provided buffer sequence. The
        operation completes when at least one byte has been read,
        or an error occurs.

        This non-virtual template wrapper satisfies the `capy::Stream`
        concept by delegating to the virtual `do_read_some`.

        @par Thread Safety
        May run concurrently with one operation in the other
        direction, subject to the class-level threading contract.
        Two concurrent operations in the same direction are
        undefined.

        @param buffers The buffer sequence to read data into.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    template<capy::MutableBufferSequence Buffers>
    [[nodiscard]] auto read_some(Buffers const& buffers)
    {
        return do_read_some(buffers);
    }

    /** Initiate an asynchronous write operation.

        Encrypts and writes data from the provided buffer sequence.
        The operation completes when at least one byte has been
        written, or an error occurs.

        This non-virtual template wrapper satisfies the `capy::Stream`
        concept by delegating to the virtual `do_write_some`.

        @par Thread Safety
        May run concurrently with one operation in the other
        direction, subject to the class-level threading contract.
        Two concurrent operations in the same direction are
        undefined.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    template<capy::ConstBufferSequence Buffers>
    [[nodiscard]] auto write_some(Buffers const& buffers)
    {
        return do_write_some(buffers);
    }

    /** Asynchronously perform the TLS handshake.

        Initiates the TLS handshake process. For client connections,
        this sends the ClientHello and processes the server's response.
        For server connections, this waits for the ClientHello and
        sends the server's response.

        A handshake attempt, successful or not, consumes the stream
        state: a subsequent call behaves as if `reset()` had been
        called first and performs a fresh handshake using the
        current configuration.

        @par Preconditions
        The underlying stream must be connected. No other TLS
        operation may be in progress on this stream.

        @param role The handshake role, client or server.

        @return An awaitable yielding `(error_code)`.
    */
    [[nodiscard]] virtual capy::io_task<> handshake(tls_role role) = 0;

    /** Asynchronously perform a graceful TLS shutdown.

        Initiates the TLS shutdown sequence by sending a close_notify
        alert and waiting for the peer's close_notify response.

        @par Preconditions
        A handshake must have completed successfully. May overlap
        a pending read. No concurrent write may be in progress.

        @par Postconditions
        If the transport ends before the peer's close_notify is
        received, the result is `capy::error::stream_truncated`, not
        success: an unannounced close is indistinguishable from a
        truncation attack and must not be reported as a clean
        shutdown. A shutdown stopped mid-flight reports canceled;
        any other transport error propagates unchanged.

        @return An awaitable yielding `(error_code)`.
    */
    [[nodiscard]] virtual capy::io_task<> shutdown() = 0;

    /** Reset TLS session state for reuse.

        Releases TLS session state including session keys and peer
        certificates, returning the stream to a state where
        `handshake()` can be called again. Internal memory
        allocations (I/O buffers) are preserved.

        Calling `handshake()` on a previously-used stream
        implicitly performs a reset first, so explicit calls
        are only needed to eagerly release session state.

        @par Preconditions
        No TLS operation (handshake, read, write, shutdown) is
        in progress.

        @par Thread Safety
        Not thread safe. The caller must ensure no concurrent
        operations are in progress on this stream.

        @note If called mid-session before `shutdown()`, pending
            TLS data is discarded and the peer will observe a
            truncated stream.
    */
    virtual void reset() = 0;

    /** Set the peer hostname for SNI and certificate verification.

        Configures the hostname sent in the TLS Server Name
        Indication extension and matched against the peer
        certificate during verification. The value takes effect
        at the next `handshake()`; an established session is not
        affected. It persists across `reset()`, so a stream reused
        to reach a different host must set the new name before
        handshaking again.

        An empty hostname (the default) disables SNI and hostname
        verification.

        If `hostname` is an IP literal (IPv4 or IPv6), it is matched
        against the certificate's iPAddress entries instead of its
        DNS names, and no SNI is sent (RFC 6066 excludes literals).
        A backend build that cannot match iPAddress entries fails the
        handshake with `std::errc::function_not_supported` rather
        than skip verification.

        @par Postconditions
        The next `handshake()` uses `hostname` for SNI and
        certificate verification, or neither if it is empty.

        @note The hostname is used for client handshakes only;
        it is ignored when handshaking as a server.

        @param hostname The peer hostname, or empty to disable.
    */
    virtual void set_hostname(std::string_view hostname) = 0;

    /** Return a reference to the underlying stream.

        Provides access to the type-erased underlying stream for
        operations like cancellation or accessing native handles.

        @warning Do not reseat (assign to) the returned reference.
            The TLS implementation holds internal state bound to
            the original stream. Replacing it causes undefined
            behavior.

        @return Reference to the wrapped stream.
    */
    virtual capy::any_stream& next_layer() noexcept = 0;

    /** Return a const reference to the underlying stream.

        @return Const reference to the wrapped stream.
    */
    virtual capy::any_stream const& next_layer() const noexcept = 0;

    /** Return the name of the TLS backend.

        @return A string identifying the TLS implementation,
            such as "openssl" or "wolfssl".
    */
    virtual std::string_view name() const noexcept = 0;

    /** Return the ALPN protocol negotiated during the handshake.

        Application-Layer Protocol Negotiation selects a single
        application protocol (for example `"h2"` or `"http/1.1"`)
        during the TLS handshake, from the list supplied via
        @ref tls_context::set_alpn.

        @return The negotiated protocol, or an empty view if no
            protocol was negotiated, ALPN was not offered, the
            handshake has not completed, or the backend/build does
            not support ALPN.

        @par Thread Safety
        Safe to call after the handshake completes; not safe to call
        concurrently with a handshake or reset.
    */
    virtual std::string_view alpn_protocol() const noexcept { return {}; }

protected:
    tls_stream() = default;

    /** Virtual read implementation.

        Derived classes override this to perform TLS decryption
        and read operations.

        @param buffers Buffer sequence to read into.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    virtual capy::io_task<std::size_t> do_read_some(
        capy::detail::mutable_buffer_array<capy::detail::max_iovec_> buffers) = 0;

    /** Virtual write implementation.

        Derived classes override this to perform TLS encryption
        and write operations.

        @param buffers Buffer sequence to write from.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    virtual capy::io_task<std::size_t> do_write_some(
        capy::detail::const_buffer_array<capy::detail::max_iovec_> buffers) = 0;
};

} // namespace boost::corosio

#endif
