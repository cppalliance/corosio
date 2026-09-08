//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/signal_set.hpp's
// documentation for signal_set, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/task.hpp>

#include <csignal>
#include <iostream>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// Waits for a real SIGINT/SIGTERM if ever launched; compiled, never run.
// tag::wait_for_shutdown[]
capy::task<>
wait_for_shutdown(corosio::io_context& ctx)
{
    corosio::signal_set signals(ctx, SIGINT, SIGTERM);

    auto [ec, signum] = co_await signals.wait();
    if (ec == capy::cond::canceled)
        co_return;
    if (!ec)
        std::cout << "Received signal " << signum << "\n";
}
// end::wait_for_shutdown[]

} // namespace
