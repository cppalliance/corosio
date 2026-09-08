//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::join_group_v4, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::join_group_v4[]
void
receive_an_ipv4_multicast_group(corosio::io_context& ioc)
{
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        return; // report the error

    // Lets other listeners on this host bind the same port and receive the
    // same group. set_option reports failure by throwing, not by returning
    // a code.
    sock.set_option(corosio::socket_option::reuse_address(true));

    // Bind before joining: a membership attaches to the socket's local port,
    // so there is nothing for the join to attach to until the bind succeeds.
    if (auto ec =
            sock.bind(corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        return; // report the error

    // 239.0.0.0/8 is the administratively scoped range, the IPv4 counterpart
    // of a private address range. The optional second argument names the
    // local interface to receive on; the default, 0.0.0.0, lets the kernel
    // choose one.
    sock.set_option(
        corosio::socket_option::join_group_v4(
            corosio::ipv4_address("239.255.0.1")));
}
// end::join_group_v4[]

} // namespace
