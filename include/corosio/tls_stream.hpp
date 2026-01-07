//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef COROSIO_TLS_STREAM_HPP
#define COROSIO_TLS_STREAM_HPP

#include <capy/task.hpp>

#include <utility>

namespace corosio {

/** A TLS stream adapter that wraps another stream.

    This class wraps a stream and provides an async_read_some
    operation that invokes the wrapped stream's async_read_some
    once, simulating TLS record layer behavior.

    @tparam Stream The stream type to wrap.
*/
template<class Stream>
struct tls_stream
{
    Stream stream_;

    template<class... Args>
    explicit tls_stream(Args&&... args) : stream_(std::forward<Args>(args)...)
    {}

    auto get_executor() const { return stream_.get_executor(); }

    capy::task async_read_some() { co_await stream_.async_read_some(); }

    template<class Stream2 = Stream>
        requires requires(Stream2& s) { s.get_frame_allocator(); }
    auto& get_frame_allocator()
    {
        return stream_.get_frame_allocator();
    }
};

} // namespace corosio

#endif
