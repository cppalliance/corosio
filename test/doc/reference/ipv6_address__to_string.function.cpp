//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/ipv6_address.hpp's
// documentation for ipv6_address::to_string, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/ipv6_address.hpp>

#include <cassert>

namespace corosio = boost::corosio;

namespace {

// tag::to_string[]
void
print_as_colon_hex()
{
    corosio::ipv6_address::bytes_type b = {
        {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}};
    corosio::ipv6_address a(b);
    assert(a.to_string() == "1:2:3:4:5:6:7:8");
}
// end::to_string[]

} // namespace
