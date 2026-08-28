//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::load_verify_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::load_verify_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void trust_a_private_ca(corosio::tls_context& ctx)
{
    // One or more concatenated PEM certificates. This adds to the trust
    // store rather than replacing it: call set_default_verify_paths too to
    // keep trusting the public CAs alongside a private one.
    if (auto ec = ctx.load_verify_file("/etc/pki/internal-ca.pem"))
        return;  // report the error

    // Trust anchors alone change nothing. The default mode is
    // tls_verify_mode::none, under which no chain is ever checked.
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return;  // report the error
}
// end::load_verify_file[]

} // namespace
