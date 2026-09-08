//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/host_name.hpp's
// documentation for host_name, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/host_name.hpp>

#include <iostream>

namespace corosio = boost::corosio;

namespace {

// tag::host_name[]
void
print_local_host_name()
{
    auto [ec, h] = corosio::host_name();
    if (ec)
        return;
    std::cout << "running on " << h << "\n";
}
// end::host_name[]

} // namespace
