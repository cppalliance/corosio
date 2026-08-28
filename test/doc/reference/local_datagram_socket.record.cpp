//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/local_datagram_socket.hpp's documentation for
// local_datagram_socket, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays
// outside the tags.
//
// local_datagram_socket's whole class body is wrapped in
// #if BOOST_COROSIO_POSIX in its own header (Windows has no AF_UNIX
// SOCK_DGRAM support), so the region that names the type is guarded the
// same way, following test/doc/snippets/4p_unix_sockets.cpp. The includes
// below are safe unconditionally -- the header itself resolves to nothing
// off POSIX.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

#if BOOST_COROSIO_POSIX
// tag::connectionless_and_connected[]
capy::task<> connectionless_and_connected(corosio::io_context& ioc)
{
    // Connectionless
    corosio::local_datagram_socket sender(ioc);
    if (auto ec = sender.open())
        co_return;
    if (auto ec = sender.bind(corosio::local_endpoint("/tmp/sender.sock")))
        co_return;
    auto [ec, n] = co_await sender.send_to(
        capy::const_buffer("hello", 5),
        corosio::local_endpoint("/tmp/receiver.sock"));
    if (ec)
        co_return;

    // Connected
    corosio::local_datagram_socket sock(ioc);
    auto [cec] = co_await sock.connect(corosio::local_endpoint("/tmp/peer.sock"));
    if (cec)
        co_return;
    auto [ec2, n2] = co_await sock.send(
        capy::const_buffer("hi", 2));
    if (ec2)
        co_return;
}
// end::connectionless_and_connected[]
#endif // BOOST_COROSIO_POSIX

} // namespace
