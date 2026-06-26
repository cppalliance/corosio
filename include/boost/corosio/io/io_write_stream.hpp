//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_WRITE_STREAM_HPP
#define BOOST_COROSIO_IO_IO_WRITE_STREAM_HPP

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

/** Abstract base for streams that support async writes.

    Provides the `write_some` operation via a pure virtual
    `do_write_some` dispatch point. Concrete classes override
    `do_write_some` to route through their implementation.

    Uses virtual inheritance from @ref io_object so that
    @ref io_stream can combine this with @ref io_read_stream
    without duplicating the `io_object` base.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe.

    @see io_read_stream, io_stream, io_object
*/
class BOOST_COROSIO_DECL io_write_stream : virtual public io_object
{
protected:
    /// Awaitable for async write operations.
    template<class ConstBufferSequence>
    struct write_some_awaitable
        : detail::bytes_op_base<write_some_awaitable<ConstBufferSequence>>
    {
        io_write_stream& ios_;
        ConstBufferSequence buffers_;

        write_some_awaitable(
            io_write_stream& ios, ConstBufferSequence buffers) noexcept
            : ios_(ios), buffers_(std::move(buffers)) {}

        std::coroutine_handle<> dispatch(
            std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return ios_.do_write_some(
                h, ex, buffers_, this->token_, &this->ec_, &this->bytes_);
        }
    };

    /** Dispatch a write through the concrete implementation.

        @param h Coroutine handle to resume on completion.
        @param ex Executor for dispatching the completion.
        @param buffers Source buffer sequence.
        @param token Stop token for cancellation.
        @param ec Output error code.
        @param bytes Output bytes transferred.

        @return Coroutine handle to resume immediately.
    */
    virtual std::coroutine_handle<> do_write_some(
        std::coroutine_handle<>,
        capy::executor_ref,
        buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) = 0;

    io_write_stream() noexcept = default;

    io_write_stream(io_write_stream&&) noexcept            = default;
    io_write_stream& operator=(io_write_stream&&) noexcept = delete;
    io_write_stream(io_write_stream const&)                = delete;
    io_write_stream& operator=(io_write_stream const&)     = delete;

public:
    /** Asynchronously write data to the stream.

        Suspends the calling coroutine and initiates a kernel-level
        write. The coroutine resumes when at least one byte is written,
        an error occurs, or the operation is cancelled.

        This stream must outlive the returned awaitable. The memory
        referenced by @p buffers must remain valid until the operation
        completes.

        @param buffers The buffer sequence containing data to write.

        @return An awaitable yielding `(error_code, std::size_t)`.

        @see io_stream::read_some
    */
    template<capy::ConstBufferSequence CB>
    auto write_some(CB const& buffers)
    {
        return write_some_awaitable<CB>(*this, buffers);
    }
};

} // namespace boost::corosio

#endif
