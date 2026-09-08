//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_local_stream_socket.hpp's
// documentation for native_local_stream_socket, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.
//
// native_local_stream_socket is a class template (`template<auto Backend>`);
// the reference slug drops the template parameter, but the example must
// still name a concrete backend tag. corosio::epoll is what this library
// actually offers as a compile-time tag on Linux (see backend.hpp); every
// backend tag defines local_stream_socket_type, so the type itself is not
// the constraint -- the tag's own existence is. Guarding on
// BOOST_COROSIO_HAS_EPOLL (rather than BOOST_COROSIO_POSIX) matches
// native_io_context.record.cpp's precedent for this exact class of example.

#include "../doc_warnings.hpp"

#include <boost/corosio/backend.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_local_stream_socket.hpp>

#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

#if BOOST_COROSIO_HAS_EPOLL
// tag::connect[]
capy::task<>
connect_native()
{
    corosio::native_io_context<corosio::epoll> ctx;
    corosio::native_local_stream_socket<corosio::epoll> s(ctx);
    auto [ec] = co_await s.connect(corosio::local_endpoint("/tmp/my.sock"));
    if (ec)
        co_return;
}
// end::connect[]
#endif // BOOST_COROSIO_HAS_EPOLL

} // namespace
