//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context::set_servername_callback, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>

#include <string_view>

namespace corosio = boost::corosio;

namespace {

// tag::set_servername_callback[]
// Configure before any stream is created from ctx; modifying a context
// afterwards is undefined behavior.
void
accept_only_known_hostnames(corosio::tls_context& ctx)
{
    // Server side: invoked during the handshake with the name the client
    // asked for. Returning false rejects the connection with an alert.
    // Write it as an allow-list, so a name this deployment does not serve
    // is refused rather than answered with whatever certificate this
    // context happens to hold.
    ctx.set_servername_callback([](std::string_view hostname) -> bool {
        return hostname == "api.example.com" || hostname == "www.example.com";
    });
}
// end::set_servername_callback[]

} // namespace
