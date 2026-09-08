//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::multicast_hops_v4, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::multicast_hops_v4[]
void
limit_how_far_multicast_travels_v4(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v4().
    //
    // The TTL is a router budget, not a distance: 1 (the default) keeps the
    // datagram on the local link, 4 lets it cross four routers. Group
    // addresses carry an administrative scope of their own -- 239.0.0.0/8 is
    // the scoped range -- and a datagram has to satisfy both.
    sock.set_option(corosio::socket_option::multicast_hops_v4(4));
}
// end::multicast_hops_v4[]

} // namespace
