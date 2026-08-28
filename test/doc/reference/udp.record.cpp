//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/udp.hpp's
// documentation for udp, by doc/addons/extensions/reference-snippets.lua.
// The tagged region is what the reference renders; scaffolding stays
// outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <system_error>

namespace corosio = boost::corosio;

namespace {

// tag::udp[]
std::error_code open_and_bind_a_udp_socket(corosio::io_context& ioc)
{
    corosio::udp_socket sock(ioc);
    if (auto ec = sock.open(corosio::udp::v4()))
        return ec;
    if (auto ec = sock.bind(
            corosio::endpoint(corosio::ipv4_address::any(), 9000)))
        return ec;
    return {};
}
// end::udp[]

} // namespace
