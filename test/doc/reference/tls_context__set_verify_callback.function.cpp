//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_verify_callback, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace corosio = boost::corosio;

namespace {

// tag::set_verify_callback[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior. refused_der is the exact DER encoding of
// a certificate this client will not accept -- one known compromised, whose
// issuer publishes no CRL you can reach. It is taken by value because the
// callback outlives this call.
void
reject_a_known_bad_certificate(
    corosio::tls_context& ctx, std::vector<unsigned char> refused_der)
{
    // A callback tightens the built-in checks; it does not switch
    // verification on. Under the default tls_verify_mode::none the library
    // never verifies and never calls it.
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return; // report the error

    // The callback runs once per certificate in the chain, and nothing here
    // says which one is the leaf -- so the check has to be one that is
    // correct at every depth. Rejecting a specific certificate wherever it
    // appears is such a check; "accept only this certificate" is not, and
    // written that way it would reject the CAs above the leaf. There is no
    // error code to check: the callback is recorded, and a backend that
    // cannot honor it fails the handshake rather than ignoring it.
    ctx.set_verify_callback(
        [refused = std::move(refused_der)](
            bool preverified, corosio::verify_context& vctx) -> bool {
            // preverified is the verdict the library reached for this
            // certificate, after any soft_fail revocation downgrade has
            // already been applied to it. Returning true when it is false
            // accepts a chain the library rejected; pass it through instead.
            if (!preverified)
                return false;

            // Valid only for this call -- do not retain it. Empty means the
            // backend could not supply the bytes, which leaves the check
            // below unable to run, so refuse rather than assume.
            auto der = vctx.certificate();
            if (der.empty())
                return false;

            return !std::equal(
                der.begin(), der.end(), refused.begin(), refused.end());
        });
}
// end::set_verify_callback[]

} // namespace
