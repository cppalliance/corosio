//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_socket.hpp's
// documentation for tcp_socket::set_option, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::set_option[]
// Precondition: sock is open (set_option throws bad_file_descriptor
// otherwise). set_option itself throws rather than returning an error
// code.
void configure_low_latency(corosio::tcp_socket& sock)
{
    sock.set_option(corosio::socket_option::no_delay(true));
    sock.set_option(corosio::socket_option::receive_buffer_size(65536));
}
// end::set_option[]

} // namespace
