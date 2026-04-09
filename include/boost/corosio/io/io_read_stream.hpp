//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_READ_STREAM_HPP
#define BOOST_COROSIO_IO_IO_READ_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/op_base.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

/** Abstract base for streams that support async reads.

    Provides the `read_some` operation via a pure virtual
    `do_read_some` dispatch point. Concrete classes override
    `do_read_some` to route through their implementation.

    Uses virtual inheritance from @ref io_object so that
    @ref io_stream can combine this with @ref io_write_stream
    without duplicating the `io_object` base.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @see io_write_stream, io_stream, io_object
*/
class BOOST_COROSIO_DECL io_read_stream : virtual public io_object
{
protected:
    /// Awaitable for async read operations.
    template<class MutableBufferSequence>
    struct read_some_awaitable
        : detail::bytes_op_base<read_some_awaitable<MutableBufferSequence>>
    {
        io_read_stream& ios_;
        MutableBufferSequence buffers_;

        read_some_awaitable(
            io_read_stream& ios, MutableBufferSequence buffers) noexcept
            : ios_(ios), buffers_(std::move(buffers)) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return ios_.do_read_some(
                h, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

    /** Dispatch a read through the concrete implementation.

        @param h Coroutine handle to resume on completion.
        @param ex Executor for dispatching the completion.
        @param buffers Target buffer sequence.
        @param token Stop token for cancellation.
        @param ec Output error code.
        @param bytes Output bytes transferred.

        @return Coroutine handle to resume immediately.
    */
    virtual std::coroutine_handle<> do_read_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) = 0;

    io_read_stream() noexcept = default;

    /// Construct from a handle.
    explicit io_read_stream(handle h) noexcept : io_object(std::move(h)) {}

    io_read_stream(io_read_stream&&) noexcept            = default;
    io_read_stream& operator=(io_read_stream&&) noexcept = delete;
    io_read_stream(io_read_stream const&)                = delete;
    io_read_stream& operator=(io_read_stream const&)     = delete;

public:
    /** Asynchronously read data from the stream.

        Suspends the calling coroutine and initiates a kernel-level
        read. The coroutine resumes when at least one byte is read,
        an error occurs, or the operation is cancelled.

        This stream must outlive the returned awaitable. The memory
        referenced by @p buffers must remain valid until the operation
        completes.

        @param buffers The buffer sequence to read data into.

        @par Cancellation
        Supports cancellation via the awaitable's stop_token or by
        calling the owning stream's `cancel()` member. On cancellation,
        yields `errc::operation_canceled`.

        @return An awaitable yielding `(error_code, std::size_t)`.

        @see io_stream::write_some
    */
    template<capy::MutableBufferSequence MB>
    auto read_some(MB const& buffers)
    {
        return read_some_awaitable<MB>(*this, buffers);
    }
};

} // namespace boost::corosio

#endif
