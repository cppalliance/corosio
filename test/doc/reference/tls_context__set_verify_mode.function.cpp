//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_verify_mode, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_verify_mode[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void verify_the_peer(corosio::tls_context& ctx)
{
    // Verification needs trust anchors to verify against.
    if (auto ec = ctx.set_default_verify_paths())
        return;  // report the error

    // peer: check the certificate the peer presents, and fail the handshake
    // if the chain does not verify. This is what a client wants, because a
    // server always presents one. A server doing mutual TLS wants
    // require_peer instead, which additionally fails when the client
    // presents no certificate at all. The default, tls_verify_mode::none,
    // checks nothing and accepts any peer.
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return;  // report the error
}
// end::set_verify_mode[]

} // namespace
