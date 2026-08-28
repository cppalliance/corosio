//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tls_context.hpp's
// documentation for tls_context, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/tls_stream.hpp>

#include <boost/capy/task.hpp>

#include <string_view>
#include <system_error>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::tls_context[]
// A default-constructed context verifies nothing: tls_verify_mode::none is
// the default. These two calls are what make a client context safe. A factory
// has no error code to return alongside the context, so it throws; the
// members themselves report failure by returning std::error_code.
corosio::tls_context make_verified_client_context()
{
    corosio::tls_context ctx;
    if (auto ec = ctx.set_default_verify_paths())
        throw std::system_error(ec);
    if (auto ec = ctx.set_verify_mode(corosio::tls_verify_mode::peer))
        throw std::system_error(ec);
    return ctx;
}

// Construct a backend stream -- corosio::openssl_stream or
// corosio::wolfssl_stream -- over a connected socket and that context, then
// hand it here: the context's settings are captured when the stream is
// built, and every backend handshakes through this same interface.
capy::task<> handshake_as_client(
    corosio::tls_stream& secure, std::string_view hostname)
{
    // Sets SNI and the name the peer certificate must match. A verified
    // chain on its own says nothing about who is on the other end.
    secure.set_hostname(hostname);

    if (auto [ec] = co_await secure.handshake(corosio::tls_role::client); ec)
        co_return;  // report the error
}
// end::tls_context[]

} // namespace
