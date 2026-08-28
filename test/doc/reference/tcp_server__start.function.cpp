//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_server.hpp's
// documentation for tcp_server::start, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>

#include <chrono>

namespace corosio = boost::corosio;

namespace {

// tag::start[]
// Precondition: srv is bound and has workers, and is Stopped -- either
// fresh, or after a complete prior stop()/run()/join() cycle.
void restart_after_full_drain(corosio::io_context& ioc, corosio::tcp_server& srv)
{
    using namespace std::chrono_literals;

    srv.start();
    ioc.run_for( 1s );
    srv.stop();       // 1. Signal shutdown
    ioc.run();        // 2. Drain remaining completions
    srv.join();       // 3. Wait for accept loops

    // 4. Restart the io_context itself: draining above ran its outstanding
    //    work to zero, which stops it, so io_context::run() below would
    //    otherwise return immediately without ever running the posted
    //    accept loops.
    ioc.restart();

    // Now safe to restart
    srv.start();
    ioc.run();
}
// end::start[]

} // namespace
