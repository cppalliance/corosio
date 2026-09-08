//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/resolver.hpp's
// documentation for resolver::resolve, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// Two overloads, two named regions in one file: `resolve(host, service)`
// performs forward DNS resolution of a name into candidate endpoints
// (forward_resolve); `resolve(endpoint const&)` performs reverse DNS
// resolution of an endpoint into a hostname and service name
// (reverse_resolve). Both share the slug `resolver__resolve.function` --
// naming each region keeps the marker bound to its own overload
// regardless of visit order.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/resolver.hpp>

#include <boost/capy/task.hpp>

#include <iostream>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// Resolving a public hostname needs the network; compiled, never run.
// tag::forward_resolve[]
capy::task<>
forward_resolve(corosio::resolver& r)
{
    auto [ec, results] = co_await r.resolve("www.example.com", "https");
    if (ec)
        co_return;
}
// end::forward_resolve[]

// tag::reverse_resolve[]
capy::task<>
reverse_resolve(corosio::resolver& r)
{
    corosio::endpoint ep(corosio::ipv4_address({127, 0, 0, 1}), 80);
    auto [ec, result] = co_await r.resolve(ep);
    if (!ec)
        std::cout << result.host_name() << ":" << result.service_name();
}
// end::reverse_resolve[]

} // namespace
