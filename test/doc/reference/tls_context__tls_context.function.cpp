//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::tls_context, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::tls_context[]
void construct_a_context()
{
    // TLS 1.2 and TLS 1.3 are allowed, no credentials and no trust anchors
    // are loaded, and the verification mode is tls_verify_mode::none -- a
    // fresh context accepts any peer. It is only as safe as what you
    // configure onto it before a stream is created from it.
    corosio::tls_context ctx;
}
// end::tls_context[]

} // namespace
