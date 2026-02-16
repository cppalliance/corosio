//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_STREAM_HPP
#define BOOST_COROSIO_IO_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/corosio/io_buffer_param.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>
#include <system_error>

#include <coroutine>
#include <cstddef>
#include <stop_token>

namespace boost::corosio {

/** Platform stream with read/write operations.

    This base class provides the fundamental async read and write
    operations for kernel-level stream I/O. Derived classes wrap
    OS-specific stream implementations (sockets, pipes, etc.) and
    satisfy @ref capy::ReadStream and @ref capy::WriteStream concepts.

    @par Semantics
    Concrete classes wrap direct platform I/O completed by the kernel.
    Functions taking `io_stream&` signal "platform implementation
    required" - use this when you need actual kernel I/O rather than
    a mock or test double.

    For generic stream algorithms that work with test mocks,
    use `template<capy::Stream S>` instead of `io_stream&`.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. All calls to a single stream must be made
    from the same implicit or explicit serialization context.

    @par Example
    @code
    // Read until buffer full or EOF
    capy::task<> read_all( io_stream& stream, std::span<char> buf )
    {
        std::size_t total = 0;
        while( total < buf.size() )
        {
            auto [ec, n] = co_await stream.read_some(
                capy::buffer( buf.data() + total, buf.size() - total ) );
            if( ec == capy::cond::eof )
                break;
            if( ec.failed() )
                capy::detail::throw_system_error( ec );
            total += n;
        }
    }
    @endcode

    @see capy::Stream, capy::ReadStream, capy::WriteStream, tcp_socket
*/
class BOOST_COROSIO_DECL io_stream : public io_object
{
public:
    /** Asynchronously read data from the stream.

        This operation suspends the calling coroutine and initiates a
        kernel-level read. The coroutine resumes when the operation
        completes.

        @li The operation completes when:
        @li At least one byte has been read into the buffer sequence
        @li The peer closes the connection (EOF)
        @li An error occurs
        @li The operation is cancelled via stop token or `cancel()`

        @par Concurrency
        At most one write operation may be in flight concurrently with
        this read. No other read operations may be in flight until this
        operation completes. Note that concurrent in-flight operations
        does not imply the initiating calls may be made concurrently;
        all calls must be serialized.

        @par Cancellation
        Supports cancellation via `std::stop_token` propagated through
        the IoAwaitable protocol, or via the I/O object's `cancel()`
        member. When cancelled, the operation completes with an error
        that compares equal to `capy::cond::canceled`.

        @par Preconditions
        The stream must be open and connected.

        @param buffers The buffer sequence to read data into. The caller
            retains ownership and must ensure validity until the
            operation completes.

        @return An awaitable yielding `(error_code, std::size_t)`.
            On success, `bytes_transferred` contains the number of bytes
            read. Compare error codes to conditions, not specific values:
            @li `capy::cond::eof` - Peer closed connection (TCP FIN)
            @li `capy::cond::canceled` - Operation was cancelled

        @par Example
        @code
        // Simple read with error handling
        auto [ec, n] = co_await stream.read_some( capy::buffer( buf ) );
        if( ec == capy::cond::eof )
            co_return;  // Connection closed gracefully
        if( ec.failed() )
            capy::detail::throw_system_error( ec );
        process( buf, n );
        @endcode

        @note This operation may read fewer bytes than the buffer
            capacity. Use a loop or `capy::async_read` to read an
            exact amount.

        @see write_some, capy::async_read
    */
    template<capy::MutableBufferSequence MB>
    auto read_some(MB const& buffers)
    {
        return read_some_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the stream.

        This operation suspends the calling coroutine and initiates a
        kernel-level write. The coroutine resumes when the operation
        completes.

        @li The operation completes when:
        @li At least one byte has been written from the buffer sequence
        @li An error occurs (including connection reset by peer)
        @li The operation is cancelled via stop token or `cancel()`

        @par Concurrency
        At most one read operation may be in flight concurrently with
        this write. No other write operations may be in flight until
        this operation completes. Note that concurrent in-flight
        operations does not imply the initiating calls may be made
        concurrently; all calls must be serialized.

        @par Cancellation
        Supports cancellation via `std::stop_token` propagated through
        the IoAwaitable protocol, or via the I/O object's `cancel()`
        member. When cancelled, the operation completes with an error
        that compares equal to `capy::cond::canceled`.

        @par Preconditions
        The stream must be open and connected.

        @param buffers The buffer sequence containing data to write.
            The caller retains ownership and must ensure validity
            until the operation completes.

        @return An awaitable yielding `(error_code, std::size_t)`.
            On success, `bytes_transferred` contains the number of bytes
            written. Compare error codes to conditions, not specific
            values:
            @li `capy::cond::canceled` - Operation was cancelled
            @li `std::errc::broken_pipe` - Peer closed connection

        @par Example
        @code
        // Write all data
        std::string_view data = "Hello, World!";
        std::size_t written = 0;
        while( written < data.size() )
        {
            auto [ec, n] = co_await stream.write_some(
                capy::buffer( data.data() + written,
                              data.size() - written ) );
            if( ec.failed() )
                capy::detail::throw_system_error( ec );
            written += n;
        }
        @endcode

        @note This operation may write fewer bytes than the buffer
            contains. Use a loop or `capy::async_write` to write
            all data.

        @see read_some, capy::async_write
    */
    template<capy::ConstBufferSequence CB>
    auto write_some(CB const& buffers)
    {
        return write_some_awaitable<CB>(*this, buffers);
    }

protected:
    /// Awaitable for async read operations.
    template<class MutableBufferSequence>
    struct read_some_awaitable
    {
        io_stream& ios_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        read_some_awaitable(
            io_stream& ios, MutableBufferSequence buffers) noexcept
            : ios_(ios)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return ios_.get().read_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

    /// Awaitable for async write operations.
    template<class ConstBufferSequence>
    struct write_some_awaitable
    {
        io_stream& ios_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        write_some_awaitable(
            io_stream& ios, ConstBufferSequence buffers) noexcept
            : ios_(ios)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            return token_.stop_requested();
        }

        capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return ios_.get().write_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

public:
    /** Platform-specific stream implementation interface.

        Derived classes implement this interface to provide kernel-level
        read and write operations for each supported platform (IOCP,
        epoll, kqueue, io_uring).
    */
    struct implementation : io_object::implementation
    {
        /// Initiate platform read operation.
        virtual std::coroutine_handle<> read_some(
            std::coroutine_handle<>,
            capy::executor_ref,
            io_buffer_param,
            std::stop_token,
            std::error_code*,
            std::size_t*) = 0;

        /// Initiate platform write operation.
        virtual std::coroutine_handle<> write_some(
            std::coroutine_handle<>,
            capy::executor_ref,
            io_buffer_param,
            std::stop_token,
            std::error_code*,
            std::size_t*) = 0;
    };

protected:
    /// Construct stream from a handle.
    explicit io_stream(handle h) noexcept : io_object(std::move(h)) {}

private:
    /// Return implementation downcasted to stream interface.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
