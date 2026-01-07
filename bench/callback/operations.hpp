//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CALLBACK_OPERATIONS_HPP
#define CALLBACK_OPERATIONS_HPP

#include "detail/op.hpp"

#include <utility>

namespace callback {

/** Performs a composed read operation on a stream.

    This function performs 5 sequential read_some operations on the
    stream, simulating a composed read that continues until a complete
    message or buffer has been received.

    This demonstrates a 2-level composed operation: async_read calls
    the stream's async_read_some member function 5 times.

    @param stream The stream to read from.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_read(Stream& stream, Handler&& handler)
{
    detail::read_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

/** Performs a composed request operation on a stream.

    This function performs 10 sequential read_some operations,
    simulating a higher-level protocol operation such as reading
    an HTTP request with headers and body.

    This demonstrates a 2-level composed operation: async_request
    calls the stream's async_read_some member function 10 times.

    @param stream The stream to read from.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_request(Stream& stream, Handler&& handler)
{
    detail::request_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

/** Performs a composed session operation on a stream.

    This function performs 100 sequential async_request operations,
    simulating a complete session that handles multiple requests
    over a persistent connection.

    This demonstrates a 3-level composed operation: async_session
    calls async_request 100 times, each of which performs 10 I/O
    operations, for a total of 1000 I/O operations.

    @param stream The stream to use for the session.
    @param handler The completion handler to invoke when done.
*/
template<class Stream, class Handler>
void async_session(Stream& stream, Handler&& handler)
{
    detail::session_op<Stream, std::decay_t<Handler>>(stream, std::forward<Handler>(handler))();
}

} // namespace callback

//----------------------------------------------------------
// Deferred definitions for detail ops that call free functions

template<class Stream, class Handler>
void callback::detail::request_op<Stream, Handler>::operator()()
{
    if(count_++ < 10)
    {
        stream_->async_read_some(std::move(*this));
        return;
    }
    handler_();
}

template<class Stream, class Handler>
void callback::detail::session_op<Stream, Handler>::operator()()
{
    if(count_++ < 100)
    {
        callback::async_request(*stream_, std::move(*this));
        return;
    }
    handler_();
}

#endif
