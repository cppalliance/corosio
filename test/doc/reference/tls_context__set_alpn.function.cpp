//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_alpn, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_alpn[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
offer_http_protocols(corosio::tls_context& ctx)
{
    // Preference order, highest first: HTTP/2, falling back to HTTP/1.1.
    // Read what the peer actually chose with tls_stream::alpn_protocol
    // after the handshake rather than assuming the first entry won.
    if (auto ec = ctx.set_alpn({"h2", "http/1.1"}))
        return; // report the error
}
// end::set_alpn[]

} // namespace
