//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::multicast_interface_v4, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::multicast_interface_v4[]
void
choose_the_outgoing_interface_v4(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v4().
    //
    // IPv4 names an interface by a local address bound to it, where
    // multicast_interface_v6 takes an interface index. The default,
    // 0.0.0.0, leaves the choice to the routing table -- which on a
    // multi-homed host is rarely the interface you meant.
    sock.set_option(
        corosio::socket_option::multicast_interface_v4(
            corosio::ipv4_address("192.168.1.1")));
}
// end::multicast_interface_v4[]

} // namespace
