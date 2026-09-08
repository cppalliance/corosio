//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_tcp_socket.hpp's documentation for
// native_tcp_socket, by doc/addons/extensions/reference-snippets.lua. The
// tagged region is what the reference renders; scaffolding stays outside
// the tags.
//
// native_tcp_socket is a class template (`template<auto Backend>`); the
// reference slug drops the template parameter, but the example must still
// name a concrete backend tag. corosio::epoll is what this library actually
// offers as a compile-time tag on Linux (see backend.hpp), matching
// native_io_context.record.cpp's precedent for this exact class of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::native_tcp_socket[]
capy::task<>
connect_and_read()
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_tcp_socket<corosio::epoll> s(ctx);
    auto [ec] = co_await s.connect(
        corosio::endpoint(corosio::ipv4_address::loopback(), 8080));
    if (ec)
        co_return;

    char buf[1024];
    auto [ec2, n] =
        co_await s.read_some(capy::mutable_buffer(buf, sizeof(buf)));
    if (ec2)
        co_return;
}
// end::native_tcp_socket[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
