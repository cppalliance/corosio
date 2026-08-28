//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::reuse_port, by
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

// tag::reuse_port[]
void share_one_port_across_several_acceptors(corosio::tcp_acceptor& acc)
{
    if (auto ec = acc.open(corosio::tcp::v6()))
        return;  // report the error

    // Every acceptor that sets this -- typically one per thread or process --
    // may bind the same port at the same time, and the kernel spreads
    // incoming connections across them. reuse_address is the weaker relative:
    // it only permits rebinding a port no longer being listened on.
    //
    // set_option reports failure by throwing, not by returning a code --
    // including on a platform with no SO_REUSEPORT at all, where it throws
    // std::system_error.
    acc.set_option(corosio::socket_option::reuse_port(true));

    if (auto ec = acc.bind(
            corosio::endpoint(corosio::ipv6_address::any(), 8080)))
        return;  // report the error
    if (auto ec = acc.listen())
        return;  // report the error
}
// end::reuse_port[]

} // namespace
