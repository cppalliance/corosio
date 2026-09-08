//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::use_certificate_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::use_certificate_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
load_the_entity_credentials(corosio::tls_context& ctx)
{
    // The certificate this endpoint presents to the peer. Use
    // use_certificate_chain_file instead when intermediates must be sent
    // with it, which is the usual case for a publicly trusted certificate.
    if (auto ec = ctx.use_certificate_file(
            "server.crt", corosio::tls_file_format::pem))
        return; // report the error

    // The key this certificate was issued for. A mismatch is not detected
    // here; it surfaces as a handshake failure.
    if (auto ec = ctx.use_private_key_file(
            "server.key", corosio::tls_file_format::pem))
        return; // report the error
}
// end::use_certificate_file[]

} // namespace
