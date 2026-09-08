//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/resolver.hpp's
// documentation for resolver, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>

#include <boost/capy/task.hpp>

#include <iostream>
#include <system_error>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// Resolving a public hostname needs the network; compiled, never run.
// tag::resolver[]
capy::task<>
resolve_and_print(corosio::io_context& ioc)
{
    corosio::resolver r(ioc);

    // Using structured bindings
    auto [ec, results] = co_await r.resolve("www.example.com", "https");
    if (ec)
        co_return;

    for (auto const& entry : results)
        std::cout << entry.get_endpoint().port() << std::endl;

    // Or, to convert errors into exceptions:
    auto [ec2, results2] = co_await r.resolve("www.example.com", "https");
    if (ec2)
        throw std::system_error(ec2);
}
// end::resolver[]

} // namespace
