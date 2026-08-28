//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::add_verify_path, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::add_verify_path[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void trust_a_directory_of_cas(corosio::tls_context& ctx)
{
    // OpenSSL looks certificates up by subject-name hash, so the directory
    // must have been prepared with `openssl rehash`; WolfSSL loads every
    // file in it. A directory that cannot be read is skipped when the
    // native context is built, not reported here -- so the verify mode
    // below is what keeps an empty trust store from passing silently.
    if (auto ec = ctx.add_verify_path("/etc/ssl/certs"))
        return;  // report the error

    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        return;  // report the error
}
// end::add_verify_path[]

} // namespace
