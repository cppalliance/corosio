//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_server.hpp's
// documentation for tcp_server, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/task.hpp>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

namespace {

// tag::running_the_server[]
// Stopped -> Running: bind before start, start before run. The worker pool
// must be built against the same io_context the server itself runs on.
void run_the_server(
    corosio::io_context& ioc,
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> workers)
{
    corosio::tcp_server srv(ioc, ioc.get_executor());
    srv.set_workers(std::move(workers));
    if (auto ec = srv.bind(
            corosio::endpoint{corosio::ipv4_address::any(), 8080}))
        return;  // report the error
    srv.start();
    ioc.run();  // Blocks until all work completes
}
// end::running_the_server[]

// tag::graceful_shutdown[]
// Precondition: srv is Running, and ioc is the io_context it was started on.
// To shut down gracefully, call stop then drain the io_context.
void shut_down_gracefully(corosio::io_context& ioc, corosio::tcp_server& srv)
{
    // stop() is the only call here that may come from another context --
    // a signal handler or a timer callback typically makes it while the
    // two calls below are running on this thread.
    srv.stop();

    // Back on the thread that owns ioc: run() drains pending completions --
    // this is what actually finishes the accept loops stop() only requested
    // the end of.
    ioc.run();

    // Once ioc.run() returns:
    srv.join();  // Wait for accept loops to finish
}
// end::graceful_shutdown[]

// tag::restart_after_stop[]
// Precondition: srv is bound and has workers. The server can be restarted
// after a complete shutdown cycle; you must drain the io_context, call
// join, and restart the io_context itself before restarting the server.
void restart_after_stop(corosio::io_context& ioc, corosio::tcp_server& srv)
{
    using namespace std::chrono_literals;

    srv.start();
    ioc.run_for( 10s );   // Run for a while
    srv.stop();           // Signal shutdown

    // REQUIRED: stop() only requests the accept loops end -- it does not
    // drive them to completion itself. Only running the executor does:
    // ioc.run() is what actually finishes the loops and brings
    // active_accepts_ back to zero.
    ioc.run();            // REQUIRED: drain pending completions

    // REQUIRED: start() throws std::logic_error if a previous session's
    // accept loops have not yet reached zero; join blocks until they have.
    srv.join();           // REQUIRED: wait for accept loops

    // REQUIRED: the reactor scheduler stops itself once its outstanding
    // work reaches zero (which draining above just caused), so ioc.run()
    // below would return immediately without restart() -- the posted
    // accept loops would never actually run, and join() would then block
    // forever waiting for a completion that never happens.
    ioc.restart();         // REQUIRED: io_context must be restarted too

    // Now safe to restart
    srv.start();
    ioc.run();
}
// end::restart_after_stop[]

// tag::custom_worker[]
// A worker owns the socket it hands to each connection and is returned to
// the pool when its coroutine completes; deriving from worker_base is what
// makes an object eligible for set_workers. The executor is fetched from
// ctx_ at launch time rather than stored as a capy::any_executor: launcher
// dispatches by posting a coroutine_handle directly, which the type-erased
// any_executor has no overload for.
class my_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
public:
    my_worker(corosio::io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {
    }

    corosio::tcp_socket& socket() override { return sock_; }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(), [](corosio::tcp_socket* sock) -> capy::task<>
        {
            // handle connection using sock
            co_return;
        }(&sock_));
    }
};

auto make_workers(corosio::io_context& ctx, int n)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> v;
    v.reserve(n);
    for(int i = 0; i < n; ++i)
        v.push_back(std::make_unique<my_worker>(ctx));
    return v;
}

void build_a_worker_pool()
{
    corosio::io_context ioc;
    corosio::tcp_server srv(ioc, ioc.get_executor());
    srv.set_workers(make_workers(ioc, 100));
}
// end::custom_worker[]

} // namespace
