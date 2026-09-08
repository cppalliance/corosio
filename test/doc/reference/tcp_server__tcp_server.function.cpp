//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_server.hpp's
// documentation for tcp_server::tcp_server, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace corosio = boost::corosio;

namespace {

// tag::tcp_server[]
void
construct_and_start(
    corosio::io_context& ctx,
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> workers)
{
    corosio::tcp_server srv(ctx, ctx.get_executor());
    srv.set_workers(std::move(workers));
    if (auto ec =
            srv.bind(corosio::endpoint{corosio::ipv4_address::any(), 8080}))
        return; // report the error
    srv.start();
}
// end::tcp_server[]

} // namespace
