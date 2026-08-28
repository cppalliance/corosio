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
// native_socket_option::integer, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/udp_socket.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

namespace corosio = boost::corosio;

namespace {

// tag::integer[]
void limit_how_far_outgoing_packets_can_travel(corosio::udp_socket& sock)
{
    // Precondition: sock is open on udp::v4(). IPPROTO_IP options don't
    // apply to an AF_INET6 socket; set_option compiles either way and
    // throws at runtime on the wrong family.
    //
    // corosio wraps the multicast hop limit (multicast_hops_v4) but not the
    // plain unicast one; IP_TTL is the general-purpose option this class is
    // for. A low value keeps a datagram from leaving the local network even
    // when a route to a farther destination exists.
    using unicast_ttl_v4 =
        corosio::native_socket_option::integer<IPPROTO_IP, IP_TTL>;

    sock.set_option(unicast_ttl_v4(1));
}
// end::integer[]

} // namespace
