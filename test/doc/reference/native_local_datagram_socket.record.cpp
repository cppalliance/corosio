//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_local_datagram_socket.hpp's
// documentation for native_local_datagram_socket, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.
//
// native_local_datagram_socket<Backend> is itself wrapped in
// #if BOOST_COROSIO_POSIX in its own header (no Windows/IOCP backend --
// iocp_t has no local_datagram_socket_type in backend.hpp), and the
// reference slug drops the template parameter, so the example must also
// name a concrete backend tag that actually exists. corosio::epoll
// satisfies both constraints at once: it only exists on Linux
// (BOOST_COROSIO_HAS_EPOLL), which is always POSIX, matching
// native_io_context.record.cpp's precedent for this exact class of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_local_datagram_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::open_bind_recv[]
capy::task<> open_bind_recv()
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_local_datagram_socket<corosio::epoll> s(ctx);
    if (auto ec = s.open())
        co_return;
    if (auto ec = s.bind(corosio::local_endpoint("/tmp/recv.sock")))
        co_return;

    char buf[1024];
    corosio::local_endpoint sender;
    auto [ec, n] = co_await s.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    if (ec)
        co_return;
}
// end::open_bind_recv[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
