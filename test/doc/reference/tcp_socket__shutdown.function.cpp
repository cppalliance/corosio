//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_socket.hpp's
// documentation for tcp_socket::shutdown, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// This block illustrates the read side's view of a peer's shutdown, not a
// call to shutdown() itself -- the docstring's point is what a subsequent
// read sees once the peer has shut down (or closed) its send direction.

#include "../doc_warnings.hpp"

#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::shutdown[]
// Precondition: sock is a connected socket.
capy::task<>
stop_reading_after_peer_shutdown(
    corosio::tcp_socket& sock, capy::mutable_buffer buf)
{
    auto [ec, n] = co_await sock.read_some(buf);

    if (ec == capy::cond::eof)
        co_return; // Peer closed their send direction
}
// end::shutdown[]

} // namespace
