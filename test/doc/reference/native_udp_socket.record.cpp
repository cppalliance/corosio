//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_udp_socket.hpp's documentation for
// native_udp_socket, by doc/addons/extensions/reference-snippets.lua. The
// tagged region is what the reference renders; scaffolding stays outside
// the tags.
//
// native_udp_socket is a class template (`template<auto Backend>`); the
// reference slug drops the template parameter, but the example must still
// name a concrete backend tag. corosio::epoll is what this library actually
// offers as a compile-time tag on Linux (see backend.hpp), matching
// native_io_context.record.cpp's precedent for this exact class of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_udp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::native_udp_socket[]
capy::task<>
open_bind_recv()
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_udp_socket<corosio::epoll> s(ctx);
    if (auto ec = s.open())
        co_return;
    if (auto ec = s.bind(corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        co_return;

    char buf[1024];
    corosio::endpoint sender;
    auto [ec, n] =
        co_await s.recv_from(capy::mutable_buffer(buf, sizeof(buf)), sender);
    if (ec)
        co_return;
}
// end::native_udp_socket[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
