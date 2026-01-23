//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_WRITE_HPP
#define BOOST_COROSIO_WRITE_HPP

#if !defined(BOOST_COROSIO_SOURCE) && defined(BOOST_COROSIO_USE_MODULES)
import boost.corosio;
#else

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/buffers/consuming_buffers.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost {
namespace corosio {

/** Write to a stream until the buffer is empty or an error occurs.

    This function writes data from the provided buffer sequence to the
    stream. Unlike `write_some()`, this function continues writing
    until the entire buffer sequence is written or an error occurs.

    The operation supports cancellation via `std::stop_token` through
    the affine awaitable protocol. If the associated stop token is
    triggered, the operation completes immediately with
    `errc::operation_canceled`.

    @param ios The I/O stream to write to.
    @param buffers The buffer sequence containing data to write.

    @return An awaitable that completes with `io_result<std::size_t>`.
        Returns success with the total number of bytes written (equal
        to buffer_size), or an error code on failure including:
        - broken_pipe: Connection closed by peer
        - operation_canceled: Cancelled via stop_token or cancel()

    @par Preconditions
    The stream must be open and ready for writing.

    @par Example
    @code
    std::string data = "Hello, World!";
    // Using structured bindings
    auto [ec, n] = co_await corosio::write(s, capy::const_buffer(data.data(), data.size()));
    if (ec)
    {
        std::cerr << "Write error: " << ec.message() << "\n";
        co_return;
    }
    // All data has been written (n == data.size())

    // Or using exceptions
    auto bytes = (co_await corosio::write(s, buf)).value();
    @endcode

    @note This function differs from `write_some()` in that it
        guarantees to write the entire buffer sequence (unless an
        error occurs), whereas `write_some()` may return after
        writing any amount of data.
*/
template<capy::ConstBufferSequence ConstBufferSequence>
capy::task<capy::io_result<std::size_t>>
write(io_stream& ios, ConstBufferSequence const& buffers)
{
    capy::consuming_buffers<ConstBufferSequence> consuming(buffers);
    std::size_t const total_size = capy::buffer_size(buffers);
    std::size_t total_written = 0;

    while (total_written < total_size)
    {
        auto [ec, n] = co_await ios.write_some(consuming);

        if (ec)
            co_return {ec, total_written};

        if (n == 0)
            co_return {make_error_code(system::errc::broken_pipe), total_written};

        consuming.consume(n);
        total_written += n;
    }

    co_return {{}, total_written};
}

} // namespace corosio
} // namespace boost

#endif
#endif
