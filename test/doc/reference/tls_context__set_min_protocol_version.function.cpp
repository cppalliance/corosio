//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_min_protocol_version, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_min_protocol_version[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void require_tls_1_3(corosio::tls_context& ctx)
{
    // Refuse anything older than TLS 1.3. The default floor is TLS 1.2,
    // which is still appropriate for the public internet; raise it when
    // every peer this context talks to is known to speak TLS 1.3.
    if (auto ec = ctx.set_min_protocol_version(corosio::tls_version::tls_1_3))
        return;  // report the error
}
// end::set_min_protocol_version[]

} // namespace
