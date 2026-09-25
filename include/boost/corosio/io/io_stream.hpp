//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_IO_STREAM_HPP
#define BOOST_COROSIO_IO_IO_STREAM_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io/io_read_stream.hpp>
#include <boost/corosio/io/io_write_stream.hpp>
#include <boost/corosio/detail/buffer_param.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

/** Reads and writes bytes through a platform I/O backend.

    Combines @ref io_read_stream and @ref io_write_stream into
    a single bidirectional stream. The `read_some` and `write_some`
    operations are inherited from the base classes and dispatch through
    `do_read_some` / `do_write_some`. This class implements those by
    forwarding to the platform `implementation`.

    The implementation hierarchy stays linear (no diamond):
    `io_object::implementation` -> `io_stream::implementation`
    -> `tcp_socket::implementation` -> backend impl.

    @par Semantics
    Concrete classes wrap direct platform I/O completed by the kernel.
    Functions taking `io_stream&` signal that platform implementation
    is required. Use this when you need actual kernel I/O rather than
    a mock or test double.

    For generic stream algorithms that work with test mocks,
    use `template<capy::Stream S>` instead of `io_stream&`.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Unsafe. All calls to a single stream must be made
    from the same implicit or explicit serialization context.

    @par Example
    @par !example io_stream

    @see io_read_stream, io_write_stream, tcp_socket
*/
class BOOST_COROSIO_DECL io_stream
    : public io_read_stream
    , public io_write_stream
{
public:
    /** Declares the read and write operations a platform backend
        must implement.

        Derived classes implement this interface to provide kernel-level
        read and write operations for each supported platform (IOCP,
        epoll, kqueue, io_uring).
    */
    struct implementation : io_object::implementation
    {
        /** Initiate platform read operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buffers Target buffer sequence.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> read_some(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buffers,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes) = 0;

        /** Initiate platform write operation.

            @param h Coroutine handle to resume on completion.
            @param ex Executor for dispatching the completion.
            @param buffers Source buffer sequence.
            @param token Stop token for cancellation.
            @param ec Output error code.
            @param bytes Output bytes transferred.

            @return Coroutine handle to resume immediately.
        */
        virtual std::coroutine_handle<> write_some(
            std::coroutine_handle<> h,
            capy::executor_ref ex,
            buffer_param buffers,
            std::stop_token token,
            std::error_code* ec,
            std::size_t* bytes) = 0;
    };

protected:
    /// Default construct; the handle is supplied through @ref io_object.
    io_stream() noexcept = default;

    /// Construct stream from a handle.
    explicit io_stream(handle h) noexcept : io_object(std::move(h)) {}

    /** Dispatch read through implementation vtable.

        @param h Coroutine handle to resume on completion.
        @param ex Executor for dispatching the completion.
        @param buffers Target buffer sequence.
        @param token Stop token for cancellation.
        @param ec Output error code.
        @param bytes Output bytes transferred.

        @return Coroutine handle to resume immediately.
    */
    std::coroutine_handle<> do_read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buffers,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return get().read_some(h, ex, buffers, std::move(token), ec, bytes);
    }

    /** Dispatch write through implementation vtable.

        @param h Coroutine handle to resume on completion.
        @param ex Executor for dispatching the completion.
        @param buffers Source buffer sequence.
        @param token Stop token for cancellation.
        @param ec Output error code.
        @param bytes Output bytes transferred.

        @return Coroutine handle to resume immediately.
    */
    std::coroutine_handle<> do_write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buffers,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return get().write_some(h, ex, buffers, std::move(token), ec, bytes);
    }

private:
    /// Return implementation downcasted to stream interface.
    implementation& get() const noexcept
    {
        return *static_cast<implementation*>(h_.get());
    }
};

} // namespace boost::corosio

#endif
