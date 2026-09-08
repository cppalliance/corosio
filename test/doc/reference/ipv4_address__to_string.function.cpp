//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/ipv4_address.hpp's
// documentation for ipv4_address::to_string, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/ipv4_address.hpp>

#include <cassert>

namespace corosio = boost::corosio;

namespace {

// tag::to_string[]
void
print_as_dotted_decimal()
{
    assert(corosio::ipv4_address(0x01020304).to_string() == "1.2.3.4");
}
// end::to_string[]

} // namespace
