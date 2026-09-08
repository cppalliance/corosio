//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::use_private_key_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::use_private_key_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
load_the_private_key(corosio::tls_context& ctx)
{
    if (auto ec = ctx.use_certificate_chain_file("fullchain.pem"))
        return; // report the error

    // Must be the key the certificate above was issued for. If the file is
    // encrypted, install a password callback with set_password_callback
    // before this call -- there is no prompt otherwise, and the missing
    // passphrase surfaces as a handshake failure rather than an error here.
    if (auto ec = ctx.use_private_key_file(
            "privkey.pem", corosio::tls_file_format::pem))
        return; // report the error
}
// end::use_private_key_file[]

} // namespace
