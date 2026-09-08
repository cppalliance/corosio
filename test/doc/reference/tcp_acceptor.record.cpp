//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_acceptor.hpp's
// documentation for tcp_acceptor, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv6_address.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>

#include <system_error>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::convenience_construction[]
// open + SO_REUSEADDR/SO_EXCLUSIVEADDRUSE + bind + listen in one call.
// Throws std::system_error if any of those steps fails.
capy::task<>
accept_with_the_convenience_constructor(corosio::io_context& ioc)
{
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(8080));

    corosio::tcp_socket peer(ioc);
    auto [ec] = co_await acc.accept(peer);
    if (!ec)
    {
        // peer is now a connected socket
        char storage[1024];
        auto [rec, n] = co_await peer.read_some(
            capy::mutable_buffer(storage, sizeof(storage)));
        if (rec)
            co_return;
    }
}
// end::convenience_construction[]

// tag::fine_grained_setup[]
// set_option throws rather than returning an error code, unlike
// open/bind/listen. Precondition: acc is not already open -- open() is a
// no-op on an already-open acceptor, so an acceptor left over from a v4
// attempt would silently keep its v4 socket and fail later at bind().
std::error_code
open_ipv6_explicitly(corosio::io_context& ioc)
{
    corosio::tcp_acceptor acc(ioc);
    if (auto ec = acc.open(corosio::tcp::v6()))
        return ec;
    acc.set_option(corosio::socket_option::reuse_address(true));
    acc.set_option(corosio::socket_option::v6_only(true));
    if (auto ec =
            acc.bind(corosio::endpoint(corosio::ipv6_address::any(), 8080)))
        return ec;
    if (auto ec = acc.listen())
        return ec;
    return {};
}
// end::fine_grained_setup[]

} // namespace
