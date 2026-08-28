//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/socket_option.hpp's
// documentation for socket_option::send_buffer_size, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::send_buffer_size[]
void widen_the_send_buffer(corosio::tcp_socket& sock)
{
    // Room for the kernel to hold data the peer has not acknowledged yet;
    // worth raising on a high-bandwidth, high-latency path.
    sock.set_option(corosio::socket_option::send_buffer_size(65536));
}
// end::send_buffer_size[]

} // namespace
