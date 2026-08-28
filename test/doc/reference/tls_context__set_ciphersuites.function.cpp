//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_ciphersuites, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_ciphersuites[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void restrict_the_tls_1_2_cipher_suites(corosio::tls_context& ctx)
{
    // OpenSSL cipher-list syntax. ECDHE gives forward secrecy and both
    // families named here are AEAD, so this narrows TLS 1.2 to suites
    // without the weaknesses of CBC modes and static RSA key exchange. An
    // unrecognised string is not rejected here -- it surfaces as a
    // handshake failure, so change this list with a test that connects.
    if (auto ec = ctx.set_ciphersuites("ECDHE+AESGCM:ECDHE+CHACHA20"))
        return;  // report the error
}
// end::set_ciphersuites[]

} // namespace
