//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::use_pkcs12_file, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

// The example reads the passphrase with std::getenv, which the Windows CRT
// deprecates and this directory builds with warnings as errors.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <boost/corosio/tls_context.hpp>

#include <cstdlib>

namespace corosio = boost::corosio;

namespace {

// tag::use_pkcs12_file[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void load_a_pkcs12_bundle(corosio::tls_context& ctx)
{
    // A PKCS#12 bundle carries the private key, so it is encrypted. Keep
    // its passphrase out of the source and out of the shipped binary: read
    // it from the environment, a secrets service, or a prompt.
    char const* passphrase = std::getenv("TLS_PKCS12_PASSPHRASE");
    if (passphrase == nullptr)
        return;  // report the missing passphrase

    // A wrong passphrase is not reported here. The bundle is decoded when
    // the native context is first built, so it surfaces as a handshake
    // failure instead.
    if (auto ec = ctx.use_pkcs12_file("credentials.pfx", passphrase))
        return;  // report the error
}
// end::use_pkcs12_file[]

} // namespace
