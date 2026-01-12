//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_READ_HPP
#define BOOST_COROSIO_READ_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/corosio/buffers_param.hpp>
#include <boost/corosio/consuming_buffers.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <type_traits>

namespace boost {
namespace corosio {

/** Read from a stream until the buffer is full or an error occurs.

    This function reads data from the stream into the provided buffer
    sequence. Unlike `read_some()`, this function continues reading
    until the entire buffer sequence is filled, an error occurs, or
    end-of-file is reached.

    The operation supports cancellation via `std::stop_token` through
    the affine awaitable protocol. If the associated stop token is
    triggered, the operation completes immediately with
    `errc::operation_canceled`.

    @param ios The I/O stream to read from.
    @param buffers The buffer sequence to read data into.

    @return An awaitable that completes with a pair of
        `{error_code, bytes_transferred}`. Returns success with the
        total number of bytes read (equal to buffer_size unless EOF),
        or an error code on failure including:
        - connection_reset: Peer closed the connection (EOF)
        - operation_canceled: Cancelled via stop_token or cancel()

    @par Preconditions
    The stream must be open and ready for reading.

    @par Example
    @code
    char buf[1024];
    auto [ec, n] = co_await corosio::read(s, capy::mutable_buffer(buf, sizeof(buf)));
    if (ec)
    {
        if (ec == boost::system::errc::connection_reset)
            std::cout << "EOF: read " << n << " bytes\n";
        else
            std::cerr << "Read error: " << ec.message() << "\n";
        co_return;
    }
    // buf is now completely filled with n bytes (n == sizeof(buf))
    @endcode

    @note This function differs from `read_some()` in that it
        guarantees to fill the entire buffer sequence (unless an
        error or EOF occurs), whereas `read_some()` may return
        after reading any amount of data.
*/
template<capy::mutable_buffer_sequence MutableBufferSequence>
capy::task<std::pair<system::error_code, std::size_t>>
read(io_stream& ios, MutableBufferSequence const& buffers)
{
    consuming_buffers<MutableBufferSequence> consuming(buffers);
    std::size_t const total_size = capy::buffer_size(buffers);
    std::size_t total_read = 0;

    while (total_read < total_size)
    {
        auto [ec, n] = co_await ios.read_some(consuming);

        if (ec)
            co_return {ec, total_read};

        if (n == 0)
            co_return {make_error_code(system::errc::connection_reset), total_read};

        consuming.consume(n);
        total_read += n;
    }

    co_return {{}, total_read};
}

} // namespace corosio
} // namespace boost

#endif
