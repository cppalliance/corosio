//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::multicast_hops_v6, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::multicast_hops_v6[]
void
limit_how_far_multicast_travels_v6(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v6().
    //
    // The hop limit is a router budget: 1 (the default) keeps the datagram
    // on the local link, 4 lets it cross four routers. IPv6 also encodes a
    // scope in the group address itself, and a datagram has to satisfy both.
    sock.set_option(corosio::socket_option::multicast_hops_v6(4));
}
// end::multicast_hops_v6[]

} // namespace
