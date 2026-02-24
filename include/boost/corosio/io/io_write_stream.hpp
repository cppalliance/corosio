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
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/io_buffer_param.hpp>
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
    {
        io_write_stream& ios_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        write_some_awaitable(
            io_write_stream& ios, ConstBufferSequence buffers) noexcept
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
            return ios_.do_write_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
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
        io_buffer_param,
        std::stop_token,
        std::error_code*,
        std::size_t*) = 0;

    io_write_stream() noexcept = default;

    /// Construct from a handle.
    explicit io_write_stream(handle h) noexcept : io_object(std::move(h)) {}

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
