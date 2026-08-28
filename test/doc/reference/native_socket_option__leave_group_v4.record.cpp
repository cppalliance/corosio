//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_socket_option.hpp's documentation for
// native_socket_option::leave_group_v4, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::leave_group_v4[]
void stop_receiving_an_ipv4_multicast_group(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v4() and joined this group.
    //
    // Membership otherwise lasts until the socket closes. The group and the
    // interface have to match the join_group_v4 that established it --
    // attempting to leave a (group, interface) pair the kernel has no
    // membership for fails with EADDRNOTAVAIL, which set_option reports by
    // throwing.
    sock.set_option(corosio::native_socket_option::leave_group_v4(
        corosio::ipv4_address("239.255.0.1")));
}
// end::leave_group_v4[]

} // namespace
