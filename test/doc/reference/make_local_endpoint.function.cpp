//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/local_endpoint.hpp's
// documentation for make_local_endpoint, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/local_endpoint.hpp>

#include <system_error>

namespace corosio = boost::corosio;

namespace {

// tag::make_local_endpoint[]
std::error_code
socket_endpoint(corosio::local_endpoint& out)
{
    auto [ec, ep] = corosio::make_local_endpoint("/tmp/app.sock");
    if (ec)
        return ec; // filename_too_long: the path does not fit
    out = ep;
    return {};
}
// end::make_local_endpoint[]

} // namespace
