//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_socket.hpp's
// documentation for tcp_socket::connect, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::connect[]
capy::task<> connect_to_a_server(corosio::io_context& ioc, corosio::endpoint ep)
{
    // s is freshly constructed and so is not yet open: connect() only
    // opens the socket automatically when it is not already open, using
    // ep's address family; an already-open socket keeps its existing
    // descriptor and family instead.
    corosio::tcp_socket s(ioc);

    auto [ec] = co_await s.connect(ep);
    if (ec)
        co_return;

    // s is now connected.
}
// end::connect[]

} // namespace
