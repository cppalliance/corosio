//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
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
#include <boost/capy/buffers/buffer_array.hpp>
#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/io_task.hpp>

#include <cstddef>
#include <string_view>

namespace boost::corosio {

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
    Shared objects: Unsafe.

    @see openssl_stream, wolfssl_stream
*/
class BOOST_COROSIO_DECL tls_stream
{
public:
    /** Different handshake types. */
    enum handshake_type
    {
        /** Perform handshaking as a client. */
        client,

        /** Perform handshaking as a server. */
        server
    };

    /** Destructor. */
    virtual ~tls_stream() = default;

    tls_stream(tls_stream const&) = delete;
    tls_stream& operator=(tls_stream const&) = delete;

    /** Initiate an asynchronous read operation.

        Reads decrypted data into the provided buffer sequence. The
        operation completes when at least one byte has been read,
        or an error occurs.

        This non-virtual template wrapper satisfies the `capy::Stream`
        concept by delegating to the virtual `do_read_some`.

        @param buffers The buffer sequence to read data into.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    template<capy::MutableBufferSequence Buffers>
    auto read_some(Buffers const& buffers)
    {
        return do_read_some(buffers);
    }

    /** Initiate an asynchronous write operation.

        Encrypts and writes data from the provided buffer sequence.
        The operation completes when at least one byte has been
        written, or an error occurs.

        This non-virtual template wrapper satisfies the `capy::Stream`
        concept by delegating to the virtual `do_write_some`.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    template<capy::ConstBufferSequence Buffers>
    auto write_some(Buffers const& buffers)
    {
        return do_write_some(buffers);
    }

    /** Perform the TLS handshake asynchronously.

        Initiates the TLS handshake process. For client connections,
        this sends the ClientHello and processes the server's response.
        For server connections, this waits for the ClientHello and
        sends the server's response.

        @param type The type of handshaking to perform (client or server).

        @return An awaitable yielding `(error_code)`.
    */
    virtual capy::io_task<>
    handshake(handshake_type type) = 0;

    /** Perform a graceful TLS shutdown asynchronously.

        Initiates the TLS shutdown sequence by sending a close_notify
        alert and waiting for the peer's close_notify response.

        @return An awaitable yielding `(error_code)`.
    */
    virtual capy::io_task<>
    shutdown() = 0;

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
    virtual void
    reset() = 0;

    /** Returns a reference to the underlying stream.

        Provides access to the type-erased underlying stream for
        operations like cancellation or accessing native handles.

        @warning Do not reseat (assign to) the returned reference.
            The TLS implementation holds internal state bound to
            the original stream. Replacing it causes undefined
            behavior.

        @return Reference to the wrapped stream.
    */
    virtual capy::any_stream&
    next_layer() noexcept = 0;

    /** Returns a const reference to the underlying stream.

        @return Const reference to the wrapped stream.
    */
    virtual capy::any_stream const&
    next_layer() const noexcept = 0;

    /** Returns the name of the TLS backend.

        @return A string identifying the TLS implementation,
            such as "openssl" or "wolfssl".
    */
    virtual std::string_view name() const noexcept = 0;

protected:
    tls_stream() = default;

    /** Virtual read implementation.

        Derived classes override this to perform TLS decryption
        and read operations.

        @param buffers Buffer sequence to read into.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    virtual capy::io_task<std::size_t>
    do_read_some(capy::mutable_buffer_array<capy::detail::max_iovec_> buffers) = 0;

    /** Virtual write implementation.

        Derived classes override this to perform TLS encryption
        and write operations.

        @param buffers Buffer sequence to write from.

        @return An awaitable yielding `(error_code,std::size_t)`.
    */
    virtual capy::io_task<std::size_t>
    do_write_some(capy::const_buffer_array<capy::detail::max_iovec_> buffers) = 0;
};

} // namespace boost::corosio

#endif
