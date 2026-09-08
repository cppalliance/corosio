//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::join_group_v6, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::join_group_v6[]
void
receive_an_ipv6_multicast_group(corosio::io_context& ioc)
{
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v6()))
        return; // report the error

    // Lets other listeners on this host bind the same port and receive the
    // same group. set_option reports failure by throwing, not by returning
    // a code.
    sock.set_option(corosio::socket_option::reuse_address(true));

    // Bind before joining: a membership attaches to the socket's local port,
    // so there is nothing for the join to attach to until the bind succeeds.
    if (auto ec =
            sock.bind(corosio::endpoint(corosio::ipv6_address::any(), 9000)))
        return; // report the error

    // ff15::1234 is a transient, site-scoped group: the 1 marks it
    // non-permanent, the 5 sets the scope. The interface index selects which
    // link to join on; 0 lets the kernel choose, and if_nametoindex() maps a
    // name such as "eth0".
    sock.set_option(
        corosio::socket_option::join_group_v6(
            corosio::ipv6_address("ff15::1234"), 0));
}
// end::join_group_v6[]

} // namespace
