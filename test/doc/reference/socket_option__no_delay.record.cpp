//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::no_delay, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// disable_nagle_on_a_connected_socket is also the sentinel that
// .github/workflows/docs.yml greps for in the rendered HTML. It exists here and
// nowhere else, which is what makes its absence from the site mean "the
// transform did not run". Renaming it means updating that gate.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::no_delay[]
void
disable_nagle_on_a_connected_socket(corosio::tcp_socket& sock)
{
    // Send small writes immediately instead of coalescing them.
    sock.set_option(corosio::socket_option::no_delay(true));

    auto nd       = sock.get_option<corosio::socket_option::no_delay>();
    bool disabled = nd.value(); // true: Nagle's algorithm is off
}
// end::no_delay[]

} // namespace
