//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_verify_depth, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_verify_depth[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
limit_the_chain_depth(corosio::tls_context& ctx)
{
    // Reject chains with more than four intermediate certificates between
    // the peer certificate and a trusted root. The default, around 100, is
    // permissive; lower it once the deployment's actual chain depth is
    // known, since a chain that exceeds the limit fails the handshake.
    if (auto ec = ctx.set_verify_depth(4))
        return; // report the error
}
// end::set_verify_depth[]

} // namespace
