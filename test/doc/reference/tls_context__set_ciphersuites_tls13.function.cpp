//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_ciphersuites_tls13, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_ciphersuites_tls13[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void restrict_the_tls_1_3_cipher_suites(corosio::tls_context& ctx)
{
    // TLS 1.3 defines a small fixed set of suites, all AEAD and all
    // forward secret, so this expresses a preference among safe choices
    // rather than excluding weak ones. Suites set here are independent of
    // set_ciphersuites, which covers TLS 1.2 and below.
    if (auto ec = ctx.set_ciphersuites_tls13(
            "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"))
        return;  // report the error
}
// end::set_ciphersuites_tls13[]

} // namespace
