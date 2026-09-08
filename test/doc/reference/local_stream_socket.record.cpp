//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/local_stream_socket.hpp's documentation for
// local_stream_socket, by doc/addons/extensions/reference-snippets.lua. The
// tagged region is what the reference renders; scaffolding stays outside the
// tags.
//
// local_stream_socket is not gated behind BOOST_COROSIO_POSIX -- it has a
// Windows (IOCP) backend (native/detail/iocp/win_local_stream_socket.hpp),
// so this region needs no platform guard.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::connect_and_read[]
capy::task<>
connect_and_read(corosio::io_context& ioc)
{
    corosio::local_stream_socket s(ioc);

    auto [ec] = co_await s.connect(corosio::local_endpoint("/tmp/my.sock"));
    if (ec)
        co_return;

    char buf[1024];
    auto [read_ec, n] =
        co_await s.read_some(capy::mutable_buffer(buf, sizeof(buf)));
    if (read_ec)
        co_return;
}
// end::connect_and_read[]

} // namespace
