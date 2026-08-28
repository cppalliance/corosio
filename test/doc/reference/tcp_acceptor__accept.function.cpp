//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_acceptor.hpp's
// documentation for tcp_acceptor::accept, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// Two overloads, two named regions in one file: `accept(tcp_socket&)` fills
// a socket the caller already owns and can reuse across connections
// (accept_into_a_reused_socket); `accept()` returns a fresh socket with no
// caller-owned socket to reuse (accept_returning_a_new_socket). Both share
// the slug
// `tcp_acceptor__accept.function` -- naming each region keeps the marker
// bound to its own overload regardless of visit order.

#include "../doc_warnings.hpp"

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::accept_into_a_reused_socket[]
// Precondition: acc is open, bound, and listening. peer must share acc's
// execution context; constructing it from acc.context() ties the two
// structurally instead of leaving the pairing to be asserted in prose.
capy::task<> accept_into_a_reused_socket(corosio::tcp_acceptor& acc)
{
    // The caller owns peer and can accept into it repeatedly -- its
    // lifetime outlives any single connection, unlike the value-returning
    // overload's socket, which is fresh on every call. This is the case
    // tcp_server::worker_base is built on: one socket per worker, reused
    // for each connection it handles in turn.
    corosio::tcp_socket peer(acc.context());

    for (;;)
    {
        auto [ec] = co_await acc.accept(peer);
        if (ec)
            co_return;

        char msg[] = "ping";
        auto [wec, n] = co_await peer.write_some(
            capy::const_buffer(msg, 4));
        if (wec)
            co_return;

        peer.close();  // ready to accept the next connection into peer
    }
}
// end::accept_into_a_reused_socket[]

// tag::accept_returning_a_new_socket[]
// Precondition: acc is open, bound, and listening.
capy::task<> accept_returning_a_new_socket(corosio::tcp_acceptor& acc)
{
    // Each call returns a fresh socket sharing acc's execution context --
    // there is no caller-owned socket to reuse, unlike accept(tcp_socket&).
    auto [ec, peer] = co_await acc.accept();
    if (ec)
        co_return;

    char msg[] = "ping";
    auto [wec, n] = co_await peer.write_some(
        capy::const_buffer(msg, 4));
    if (wec)
        co_return;
}
// end::accept_returning_a_new_socket[]

} // namespace
