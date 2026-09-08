//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_socket.hpp's
// documentation for tcp_socket::get_option, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// Round 1 review: the first fix here set no_delay before reading it back, to
// make the shipped `// true: Nagle's algorithm is off` comment true (see the
// git history for that finding -- it stands, TCP_NODELAY is never set
// automatically). But that made this page a near-duplicate of
// socket_option__no_delay.record.cpp's own round-trip example. This version
// instead teaches what is specific to get_option as a member: Option is an
// explicit template argument, never deduced, and the call throws rather than
// returning an error code.

#include "../doc_warnings.hpp"

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::get_option[]
// Precondition: sock is open (get_option throws bad_file_descriptor
// otherwise, and throws again if the underlying getsockopt call fails).
// Option is always an explicit template argument -- it is never deduced
// from sock or from any function argument.
template<class Option>
Option
read_option(corosio::tcp_socket& sock)
{
    return sock.get_option<Option>();
}
// end::get_option[]

// Not part of the rendered page: forces an instantiation of the template
// above so its body is actually compiled, not merely parsed.
[[maybe_unused]] void
instantiate_read_option(corosio::tcp_socket& sock)
{
    read_option<corosio::socket_option::no_delay>(sock);
}

} // namespace
