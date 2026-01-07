//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef COROUTINE_OPERATIONS_HPP
#define COROUTINE_OPERATIONS_HPP

#include <capy/task.hpp>

namespace coroutine {

/** Performs a composed read operation on a stream.

    This coroutine performs 5 sequential read_some operations on the
    stream, simulating a composed read that continues until a complete
    message or buffer has been received.

    This demonstrates a 2-level composed operation: async_read calls
    the stream's async_read_some member function 5 times.

    @param stream The stream to read from.

    @return A task that completes when all read operations finish.
*/
template<class Stream>
capy::task async_read(Stream& stream)
{
    for(int i = 0; i < 5; ++i)
        co_await stream.async_read_some();
}

/** Performs a composed request operation on a stream.

    This coroutine performs 10 sequential read_some operations,
    simulating a higher-level protocol operation such as reading
    an HTTP request with headers and body.

    This demonstrates a 2-level composed operation: async_request
    calls the stream's async_read_some member function 10 times.

    @param stream The stream to read from.

    @return A task that completes when the entire request is read.
*/
template<class Stream>
capy::task async_request(Stream& stream)
{
    for(int i = 0; i < 10; ++i)
        co_await stream.async_read_some();
}

/** Performs a composed session operation on a stream.

    This coroutine performs 100 sequential async_request operations,
    simulating a complete session that handles multiple requests
    over a persistent connection.

    This demonstrates a 3-level composed operation: async_session
    calls async_request 100 times, each of which performs 10 I/O
    operations, for a total of 1000 I/O operations.

    @param stream The stream to use for the session.

    @return A task that completes when the session ends.
*/
template<class Stream>
capy::task async_session(Stream& stream)
{
    for(int i = 0; i < 100; ++i)
        co_await async_request(stream);
}

} // namespace coroutine

#endif

