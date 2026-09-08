//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/local_stream.hpp's
// documentation for local_stream, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// local_stream is a plain protocol tag with no platform-specific member --
// unlike local_datagram, it is not gated behind BOOST_COROSIO_POSIX in its
// own header, and local_stream_socket ships a Windows (IOCP) backend too
// (native/detail/iocp/win_local_stream_socket.hpp), so this region needs
// no platform guard.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_stream.hpp>
#include <boost/corosio/local_stream_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::open_with_protocol[]
void
open_with_protocol(corosio::io_context& ctx)
{
    corosio::local_stream_socket sock(ctx);
    if (auto ec = sock.open(corosio::local_stream{}))
        return;
}
// end::open_with_protocol[]

} // namespace
