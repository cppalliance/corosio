//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::multicast_loop_v6, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::multicast_loop_v6[]
void
loop_multicast_back_to_this_host_v6(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v6().
    //
    // Enabled, datagrams this socket sends are also delivered to members of
    // the group on this same host, the sending process included. Disable it
    // when a sender must not receive its own traffic.
    sock.set_option(corosio::socket_option::multicast_loop_v6(true));
}
// end::multicast_loop_v6[]

} // namespace
