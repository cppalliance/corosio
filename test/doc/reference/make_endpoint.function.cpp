//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/endpoint.hpp's
// documentation for make_endpoint, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>

#include <cassert>

namespace corosio = boost::corosio;

namespace {

// tag::make_endpoint[]
void parse_v4_and_v6()
{
    auto [ec, ep] = corosio::make_endpoint("192.168.1.1:8080");
    if (ec)
        return;
    assert(ep.is_v4() && ep.port() == 8080);

    auto [ec6, ep6] = corosio::make_endpoint("[::1]:443");
    if (ec6)
        return;
    assert(ep6.is_v6() && ep6.port() == 443);
}
// end::make_endpoint[]

} // namespace
