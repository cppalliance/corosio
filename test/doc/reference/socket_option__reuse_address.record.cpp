//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::reuse_address, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::reuse_address[]
void restart_a_listener_on_the_same_port(corosio::tcp_acceptor& acc)
{
    if (auto ec = acc.open(corosio::tcp::v4()))
        return;  // report the error

    // Lets bind() succeed while connections from a previous listener are
    // still in TIME_WAIT -- the difference between a server that restarts
    // and one that fails with address_in_use. It does not let two live
    // listeners share a port; that is reuse_port. Must precede bind().
    // set_option reports failure by throwing, not by returning a code.
    acc.set_option(corosio::socket_option::reuse_address(true));

    if (auto ec = acc.bind(corosio::endpoint(8080)))
        return;  // report the error
    if (auto ec = acc.listen())
        return;  // report the error
}
// end::reuse_address[]

} // namespace
