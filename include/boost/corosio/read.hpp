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
#include <boost/capy/io_result.hpp>
#include <boost/capy/buffers/consuming_buffers.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/task.hpp>
#include <boost/system/error_code.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <string>
#include <type_traits>

namespace boost::corosio {

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

    @return An awaitable that completes with `io_result<std::size_t>`.
        Returns success with the total number of bytes read (equal to
        buffer_size unless EOF), or an error code on failure including:
        - capy::error::eof: End of stream reached
        - operation_canceled: Cancelled via stop_token or cancel()

    @par Preconditions
    The stream must be open and ready for reading.

    @par Example
    @code
    char buf[1024];
    auto [ec, n] = co_await corosio::read(s, capy::mutable_buffer(buf, sizeof(buf)));
    if (ec)
    {
        if (ec == capy::error::eof)
            std::cout << "EOF: read " << n << " bytes\n";
        else
            std::cerr << "Read error: " << ec.message() << "\n";
        co_return;
    }
    @endcode

    @note This function differs from `read_some()` in that it
        guarantees to fill the entire buffer sequence (unless an
        error or EOF occurs), whereas `read_some()` may return
        after reading any amount of data.
*/
template<capy::MutableBufferSequence MB>
capy::task<capy::io_result<std::size_t>>
read(io_stream& ios, MB bs)
{
    capy::consuming_buffers<MB> consuming(bs);
    std::size_t const total_size = capy::buffer_size(bs);
    std::size_t total_read = 0;

    while (total_read < total_size)
    {
        auto [ec, n] = co_await ios.read_some(consuming);
        if (ec)
            co_return {ec, total_read};
        if (n == 0)
            co_return {make_error_code(capy::error::eof), total_read};
        consuming.consume(n);
        total_read += n;
    }

    co_return {{}, total_read};
}

/** Read from a stream into a string until EOF or an error occurs.

    This function appends data from the stream to the provided string,
    automatically growing the string as needed until end-of-file is
    reached or an error occurs. Existing content in the string is
    preserved.

    The string is resized to accommodate incoming data using a growth
    strategy that starts with 2048 bytes of additional capacity and
    grows by a factor of 1.5 when the buffer fills. Before returning,
    the string is resized to contain exactly the data read (plus any
    original content).

    The operation supports cancellation via `std::stop_token` through
    the affine awaitable protocol. If the associated stop token is
    triggered, the operation completes immediately with
    `errc::operation_canceled`.

    @param ios The I/O stream to read from.
    @param s The string to append data to. Existing content is preserved.

    @return An awaitable that completes with `io_result<std::size_t>`.
        The `n` value represents only the new bytes read, not including
        any content that was already in the string. Returns:
        - capy::error::eof with bytes read: End of stream reached
        - errc::value_too_large: String reached max_size() and more data available
        - operation_canceled: Cancelled via stop_token or cancel()
        - Other error codes: Operation failed

    @par Preconditions
    The stream must be open and ready for reading.

    @par Example
    @code
    std::string content;
    auto [ec, n] = co_await corosio::read(s, content);
    if (ec && ec != capy::error::eof)
    {
        std::cerr << "Read error: " << ec.message() << "\n";
        co_return;
    }
    std::cout << "Read " << n << " bytes, total size: " << content.size() << "\n";
    @endcode

    @note Existing string content is preserved. To read into an empty
        string, call `s.clear()` before invoking this function.
*/
inline
capy::task<capy::io_result<std::size_t>>
read(io_stream& ios, std::string& s)
{
    std::size_t const base = s.size();
    std::size_t const max_size = s.max_size();
    std::size_t capacity = s.capacity();

    // Ensure at least 2048 bytes of additional capacity
    if (capacity < base + 2048)
    {
        capacity = base + 2048;
        if (capacity > max_size)
            capacity = max_size;
    }

    if (capacity <= base)
    {
        // Already at max_size with no room to grow
        co_return {make_error_code(system::errc::value_too_large), 0};
    }

    s.resize(capacity);
    std::size_t write_pos = base;

    for (;;)
    {
        // Grow by 1.5x when buffer is full
        if (write_pos == capacity)
        {
            if (capacity == max_size)
            {
                // Cannot grow further
                s.resize(write_pos);
                co_return {make_error_code(system::errc::value_too_large),
                    write_pos - base};
            }

            // Calculate new capacity with overflow protection
            std::size_t new_capacity = capacity / 2 + capacity; // 1.5x
            if (new_capacity < capacity || new_capacity > max_size)
                new_capacity = max_size;

            capacity = new_capacity;
            s.resize(capacity);
        }

        auto [ec, n] = co_await ios.read_some(
            capy::mutable_buffer(s.data() + write_pos, capacity - write_pos));

        if (ec)
        {
            s.resize(write_pos);
            co_return {ec, write_pos - base};
        }

        if (n == 0)
        {
            s.resize(write_pos);
            co_return {make_error_code(capy::error::eof), write_pos - base};
        }

        write_pos += n;
    }
}

} // namespace boost::corosio

#endif
