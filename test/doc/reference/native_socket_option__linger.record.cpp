//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into
// include/boost/corosio/native/native_socket_option.hpp's documentation for
// native_socket_option::linger, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/tcp_socket.hpp>

namespace corosio = boost::corosio;

namespace {

// tag::linger[]
void control_what_close_does_with_queued_data(corosio::tcp_socket& sock)
{
    // A non-zero timeout can make close() block the calling thread for up
    // to that many seconds. close() also runs from the destructor and from
    // move-assignment, and in an async program the thread reaching those is
    // usually the one running the event loop -- so weigh this option
    // against stalling that loop. A zero timeout means the opposite:
    // close() discards whatever is queued and sends an RST.
    //
    // This native variant stores the platform's struct linger directly,
    // where socket_option::linger keeps the same bytes behind opaque
    // storage -- an implementation detail invisible at this call site.
    sock.set_option(corosio::native_socket_option::linger(true, 5));

    auto opt = sock.get_option<corosio::native_socket_option::linger>();
    bool waits = opt.enabled();
    int seconds = opt.timeout();
}
// end::linger[]

} // namespace
