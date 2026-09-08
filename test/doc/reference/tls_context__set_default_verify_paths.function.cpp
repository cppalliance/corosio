//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_default_verify_paths, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_default_verify_paths[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
trust_the_system_cas(corosio::tls_context& ctx)
{
    // Whether the system store actually loaded is not reported here; if it
    // could not be read the context simply trusts nothing.
    if (auto ec = ctx.set_default_verify_paths())
        return; // report the error

    // Which is why this call is not optional: under the default mode,
    // tls_verify_mode::none, an empty trust store and a full one behave
    // identically -- every peer is accepted.
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return; // report the error

    // Verification proves the chain, not the identity. Call
    // tls_stream::set_hostname before the handshake so the certificate is
    // also matched against the name being connected to.
}
// end::set_default_verify_paths[]

} // namespace
