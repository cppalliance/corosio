//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::v6_only, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::v6_only[]
void accept_ipv6_peers_only(corosio::tcp_acceptor& acc)
{
    if (auto ec = acc.open(corosio::tcp::v6()))
        return;  // report the error

    // Set between open() and bind(): once bound, the option no longer moves.
    // Disabled, an IPv6 acceptor also accepts IPv4 peers and reports them as
    // v4-mapped addresses. tcp_acceptor::open() leaves the acceptor dual-stack
    // (v6_only false) on every backend; tcp_socket and udp_socket are the
    // opposite, their open() making an IPv6 socket v6-only. Set the option
    // explicitly whenever either behavior matters, rather than relying on a
    // default that differs by object. set_option reports failure by throwing,
    // not by returning a code.
    acc.set_option(corosio::socket_option::v6_only(true));

    if (auto ec = acc.bind(
            corosio::endpoint(corosio::ipv6_address::any(), 8080)))
        return;  // report the error
    if (auto ec = acc.listen())
        return;  // report the error
}
// end::v6_only[]

} // namespace
