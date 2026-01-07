//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CALLBACK_TLS_STREAM_HPP
#define CALLBACK_TLS_STREAM_HPP

#include "detail/op.hpp"

#include <corosio/platform_reactor.hpp>
#include <corosio/io_context.hpp>

namespace callback {

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

    template<class Handler>
    void async_read_some(Handler&& handler)
    {
        detail::tls_read_op<Stream, std::decay_t<Handler>>(
            stream_, std::forward<Handler>(handler))();
    }
};

} // namespace callback

#endif

