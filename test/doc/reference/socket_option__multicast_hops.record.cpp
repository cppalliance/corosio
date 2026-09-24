//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::multicast_hops, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::multicast_hops[]
void
limit_how_far_multicast_travels(corosio::udp_socket& sock)
{
    // Works for a socket of either family; the option renders as the
    // IPv4 TTL or the IPv6 hop limit to match the socket.
    //
    // The value is a router budget, not a distance: 1 (the default) keeps
    // the datagram on the local link, 4 lets it cross four routers. Group
    // addresses also carry a scope of their own, and a datagram has to
    // satisfy both.
    sock.set_option(corosio::socket_option::multicast_hops(4));
}
// end::multicast_hops[]

} // namespace
