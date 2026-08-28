//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::use_certificate_chain_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::use_certificate_chain_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void load_a_certificate_chain(corosio::tls_context& ctx)
{
    // Concatenated PEM, leaf first, then intermediates, root omitted --
    // what an ACME client writes as fullchain.pem. Serving the leaf alone
    // leaves peers that do not already hold the intermediate unable to
    // build a chain to a trusted root.
    if (auto ec = ctx.use_certificate_chain_file("fullchain.pem"))
        return;  // report the error

    // The key the leaf certificate was issued for.
    if (auto ec = ctx.use_private_key_file(
            "privkey.pem", corosio::tls_file_format::pem))
        return;  // report the error
}
// end::use_certificate_chain_file[]

} // namespace
