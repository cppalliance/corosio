//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/udp_socket.hpp's
// documentation for udp_socket, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays
// outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::udp_socket[]
capy::task<> connectionless_and_connected_modes(corosio::io_context& ioc)
{
    // Connectionless mode: each send_to/recv_from carries an endpoint --
    // a datagram is a single addressed unit, unlike a stream's byte flow.
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        co_return;
    if (auto ec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        co_return;

    char buf[1024];
    corosio::endpoint sender;
    auto [ec, n] = co_await sock.recv_from(
        capy::mutable_buffer(buf, sizeof(buf)), sender);
    if (ec)
        co_return;
    auto [sec, sn] = co_await sock.send_to(
        capy::const_buffer(buf, n), sender);
    if (sec)
        co_return;

    // Connected mode: connect() fixes a default peer, so send/recv drop
    // the endpoint argument and the kernel filters out datagrams from
    // any other source.
    corosio::udp_socket csock(ioc);
    auto [cec] = co_await csock.connect(
        corosio::endpoint(corosio::ipv4_address::loopback(), 9000));
    if (cec)
        co_return;
    auto [wec, wn] = co_await csock.send(capy::const_buffer(buf, n));
    if (wec)
        co_return;
}
// end::udp_socket[]

} // namespace
