//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_server.hpp's
// documentation for tcp_server::join, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// tag::correct_usage[]
// Precondition: srv is bound and has workers.
void
run_server_to_completion(corosio::io_context& ioc, corosio::tcp_server& srv)
{
    // main thread
    srv.start();
    ioc.run();  // Blocks until work completes
    srv.join(); // Safe: called after ioc.run() returns
}
// end::correct_usage[]

// tag::deadlock_scenarios[]
// WRONG: calling join() from inside a worker coroutine
class self_joining_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
    corosio::tcp_server& srv_;

public:
    self_joining_worker(corosio::io_context& ctx, corosio::tcp_server& srv)
        : ctx_(ctx)
        , sock_(ctx)
        , srv_(srv)
    {
    }

    corosio::tcp_socket& socket() override
    {
        return sock_;
    }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(), [this]() -> capy::task<> {
            srv_.join(); // DEADLOCK: blocks the executor
            co_return;
        }());
    }
};
// end::deadlock_scenarios[]

} // namespace
