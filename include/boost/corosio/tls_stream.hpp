//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TLS_STREAM_HPP
#define BOOST_COROSIO_TLS_STREAM_HPP

#include <boost/capy/task.hpp>

#include <utility>

namespace boost {
namespace corosio {

/** A TLS stream adapter that wraps another stream.

    This class template wraps an underlying stream and provides a
    TLS-like interface. The current implementation is a placeholder
    that delegates to the wrapped stream without actual TLS encryption.

    @par Thread Safety
    Distinct objects: Safe.@n
    Shared objects: Unsafe.

    @par Example
    @code
    corosio::socket raw_socket(ioc);
    corosio::tls_stream<corosio::socket> secure(std::move(raw_socket));
    co_await secure.async_read_some();
    @endcode

    @tparam Stream The underlying stream type to wrap. Must provide
        `get_executor()` and `async_read_some()` member functions.
*/
template<class Stream>
struct tls_stream
{
    /** The wrapped stream object. */
    Stream stream_;

    /** Construct a TLS stream by forwarding arguments to the underlying stream.

        @param args Arguments forwarded to the Stream constructor.
    */
    template<class... Args>
    explicit tls_stream(Args&&... args) : stream_(std::forward<Args>(args)...)
    {}

    /** Return the executor associated with the underlying stream.

        @return The executor from the wrapped stream.
    */
    auto get_executor() const { return stream_.get_executor(); }

    /** Initiate an asynchronous read operation.

        This function delegates to the underlying stream's async_read_some
        operation. In a full TLS implementation, this would handle TLS
        record layer processing.

        @return A task that completes when the read operation finishes.
    */
    capy::task<> async_read_some() { co_await stream_.async_read_some(); }

    /** Return the frame allocator from the underlying stream.

        This function is only available if the underlying stream type
        provides a `get_frame_allocator()` member function.

        @return Reference to the frame allocator.
    */
    template<class Stream2 = Stream>
        requires requires(Stream2& s) { s.get_frame_allocator(); }
    auto& get_frame_allocator()
    {
        return stream_.get_frame_allocator();
    }
};

} // namespace corosio
} // namespace boost

#endif
