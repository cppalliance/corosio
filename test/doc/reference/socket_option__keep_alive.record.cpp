//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::keep_alive, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::keep_alive[]
void
detect_a_peer_that_went_away(corosio::tcp_socket& sock)
{
    // Probe an idle connection so a peer that vanished without closing is
    // eventually reported as an error instead of hanging forever.
    sock.set_option(corosio::socket_option::keep_alive(true));
}
// end::keep_alive[]

} // namespace
