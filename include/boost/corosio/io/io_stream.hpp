//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
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

/** Platform stream with read/write operations.

    Combines @ref io_read_stream and @ref io_write_stream into
    a single bidirectional stream. The `read_some` and `write_some`
    operations are inherited from the base classes and dispatch
    through `do_read_some` / `do_write_some`, which this class
    implements by forwarding to the platform `implementation`.

    The implementation hierarchy stays linear (no diamond):
    `io_object::implementation` -> `io_stream::implementation`
    -> `tcp_socket::implementation` -> backend impl.

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

    @see io_read_stream, io_write_stream, tcp_socket
*/
class BOOST_COROSIO_DECL io_stream
    : public io_read_stream
    , public io_write_stream
{
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
            buffer_param,
            std::stop_token,
            std::error_code*,
            std::size_t*) = 0;

        /// Initiate platform write operation.
        virtual std::coroutine_handle<> write_some(
            std::coroutine_handle<>,
            capy::executor_ref,
            buffer_param,
            std::stop_token,
            std::error_code*,
            std::size_t*) = 0;
    };

protected:
    io_stream() noexcept = default;

    /// Construct stream from a handle.
    explicit io_stream(handle h) noexcept : io_object(std::move(h)) {}

    /// Dispatch read through implementation vtable.
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

    /// Dispatch write through implementation vtable.
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
