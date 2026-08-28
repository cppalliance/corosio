//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/endpoint.hpp's
// documentation for endpoint, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/ipv6_address.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::endpoint[]
void construct_endpoints()
{
    // IPv4 endpoint
    corosio::endpoint ep4(corosio::ipv4_address::loopback(), 8080);

    // IPv6 endpoint
    corosio::endpoint ep6(corosio::ipv6_address::loopback(), 8080);

    // Port only (defaults to IPv4 any address)
    corosio::endpoint bind_addr(8080);

    // Create from string
    auto [ec, ep] = corosio::make_endpoint("192.168.1.1:8080");
    if (ec)
        return;
}
// end::endpoint[]

} // namespace
