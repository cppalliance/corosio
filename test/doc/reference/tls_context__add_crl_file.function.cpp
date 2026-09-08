//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::add_crl_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::add_crl_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
check_revocation_against_a_crl(corosio::tls_context& ctx)
{
    // Revocation is the last of three gates and all three have to be open.
    // First, a trust anchor to build a chain to.
    if (auto ec = ctx.load_verify_file("/etc/pki/internal-ca.pem"))
        return; // report the error

    // Second, a verify mode. Under the default tls_verify_mode::none the
    // library still reads the CRL and still marks a revoked certificate
    // bad, then completes the handshake anyway -- the verdict is computed
    // and discarded.
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return; // report the error

    // Third, the CRL itself and a policy that consults it: a CRL under the
    // default disabled policy is never read.
    if (auto ec = ctx.add_crl_file("issuer.crl"))
        return; // report the error

    // No error code to check here; the policy is recorded, and a backend
    // that cannot check revocation fails the handshake rather than
    // skipping the check. hard_fail also rejects a certificate whose status
    // could not be determined -- what a CRL that is missing, expired, or
    // from the wrong issuer produces -- so with all three gates open,
    // keeping the file fresh is an operational requirement.
    ctx.set_revocation_policy(corosio::tls_revocation_policy::hard_fail);
}
// end::add_crl_file[]

} // namespace
