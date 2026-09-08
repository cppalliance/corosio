//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/tcp_server.hpp's
// documentation for tcp_server::set_workers, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// NOTE (task-8-report.md): the plan predicted this file would cover `bind`
// (the range-constrained template it flagged sits at roughly line 654). Line
// 654 is inside set_workers's own docstring, not bind's -- bind takes a
// single endpoint and has no @code example at all. The tool that generated
// symbol-map.txt mis-parsed the declaration and printed "decltype" as the
// symbol name for the same reason a human skimming the requires-clause
// might. This file is named for the symbol the marker actually sits under.

#include "../doc_warnings.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/capy/task.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

// Minimal concrete worker, only to give set_workers's own example something
// to build a vector of. tcp_server.record.cpp's `custom_worker` region is
// the reference's full illustration of implementing a worker.
class my_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;

public:
    my_worker(corosio::io_context& ctx) : ctx_(ctx), sock_(ctx) {}

    corosio::tcp_socket& socket() override
    {
        return sock_;
    }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(
            ctx_.get_executor(), [](corosio::tcp_socket* sock) -> capy::task<> {
                co_return;
            }(&sock_));
    }
};

// tag::set_workers[]
// Precondition: none the type system enforces on srv's state, but calling
// this while srv is running discards any worker mid-connection -- the
// idle/active lists are cleared before the new pool is populated.
void
configure_the_worker_pool(corosio::io_context& ctx, corosio::tcp_server& srv)
{
    std::vector<std::unique_ptr<my_worker>> workers;
    for (int i = 0; i < 100; ++i)
        workers.push_back(std::make_unique<my_worker>(ctx));
    srv.set_workers(std::move(workers));
}
// end::set_workers[]

} // namespace
