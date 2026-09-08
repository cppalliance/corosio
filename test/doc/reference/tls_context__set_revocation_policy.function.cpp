//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_revocation_policy, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_revocation_policy[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
require_a_successful_revocation_check(corosio::tls_context& ctx)
{
    // A policy on its own enforces nothing. It needs a trust anchor to
    // build a chain to, a verify mode that makes the verdict fatal, and a
    // CRL to check against. With any of the three missing the handshake
    // completes regardless of what the CRL says.
    if (auto ec = ctx.load_verify_file("/etc/pki/internal-ca.pem"))
        return; // report the error
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return; // report the error
    if (auto ec = ctx.add_crl_file("issuer.crl"))
        return; // report the error

    // hard_fail rejects both a revoked certificate and one whose status
    // cannot be determined, so a CRL that expired or was never fetched
    // stops connections instead of quietly waving them through. There is no
    // error code to check: the policy is recorded, and a backend that cannot
    // check revocation fails the handshake rather than skipping the check.
    ctx.set_revocation_policy(corosio::tls_revocation_policy::hard_fail);

    // soft_fail is the weaker alternative. It still rejects a certificate
    // listed in a CRL, but accepts one whose status is unknown -- which
    // means a revocation you failed to fetch does not stop the connection.
    // Choose it only where the CRL cannot be kept fresh.
}
// end::set_revocation_policy[]

} // namespace
