//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::broadcast, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::broadcast[]
void allow_sending_to_a_broadcast_address(corosio::io_context& ioc)
{
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        return;  // report the error

    // Without this the kernel refuses a send_to a broadcast address; the
    // permission is opt-in so a stray destination cannot flood a segment.
    // set_option reports failure by throwing, not by returning a code.
    sock.set_option(corosio::socket_option::broadcast(true));

    // send_to may now target ipv4_address::broadcast(), 255.255.255.255,
    // or a subnet-directed broadcast address.
}
// end::broadcast[]

} // namespace
