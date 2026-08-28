//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/connect.hpp's
// documentation for the range overload of connect (the constrained
// function template `template<class Socket, std::ranges::input_range
// Range> requires std::convertible_to<...>` at connect.hpp:140), by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what
// the reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/connect.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/task.hpp>

#include <utility>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// Resolving and connecting to a public hostname needs the network;
// compiled, never run.
// tag::connect[]
capy::task<> connect_to_first_available(corosio::io_context& ioc)
{
    corosio::resolver r(ioc);
    auto [rec, results] = co_await r.resolve("www.boost.org", "80");
    if (rec)
        co_return;

    corosio::tcp_socket s(ioc);

    // std::move avoids a deep copy of results -- resolver_results owns
    // two std::strings per entry, and results is not used again below.
    auto [cec, ep] = co_await corosio::connect(s, std::move(results));
    if (cec)
        co_return;
}
// end::connect[]

} // namespace
