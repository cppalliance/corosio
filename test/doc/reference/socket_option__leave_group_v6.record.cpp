//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::leave_group_v6, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::leave_group_v6[]
void stop_receiving_an_ipv6_multicast_group(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v6() and joined this group.
    //
    // Membership otherwise lasts until the socket closes. The group and the
    // interface index have to match the join_group_v6 that established it --
    // attempting to leave a (group, interface) pair the kernel has no
    // membership for fails with EADDRNOTAVAIL, which set_option reports by
    // throwing.
    sock.set_option(corosio::socket_option::leave_group_v6(
        corosio::ipv6_address("ff15::1234"), 0));
}
// end::leave_group_v6[]

} // namespace
