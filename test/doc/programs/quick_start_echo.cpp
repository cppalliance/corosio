//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Full program assembled across pages/quick-start.adoc. Compiled and
// linked only: main() binds a port and runs forever, and the runtime
// behavior is covered by test/doc/run_examples.py against the real
// echo-server example.

// tag::assume[]
#include <boost/corosio/tcp_server.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

#include <iostream>
#include <memory>
#include <string>
#include <vector>

// tag::server_class[]
class worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
    std::string buf_;

public:
    worker(corosio::io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {
        buf_.reserve(4096);
    }

    corosio::tcp_socket& socket() override { return sock_; }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(), do_session());
    }

    capy::task<> do_session();
};

// Build a pool of workers as a range of unique_ptr<worker_base>.
inline auto
make_workers(corosio::io_context& ctx, int n)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<worker>(ctx));
    return v;
}

class echo_server : public corosio::tcp_server
{
public:
    echo_server(corosio::io_context& ctx, int max_workers)
        : tcp_server(ctx, ctx.get_executor())
    {
        set_workers(make_workers(ctx, max_workers));
    }
};
// end::server_class[]

// tag::session[]
capy::task<> worker::do_session()
{
    for (;;)
    {
        buf_.resize(4096);

        // Read some data
        auto [ec, n] = co_await sock_.read_some(
            capy::mutable_buffer(buf_.data(), buf_.size()));

        if (ec || n == 0)
            break;  // Connection closed or error

        buf_.resize(n);

        // Echo it back
        auto [wec, wn] = co_await capy::write(
            sock_, capy::const_buffer(buf_.data(), buf_.size()));

        if (wec)
            break;  // Write error
    }

    sock_.close();
}
// end::session[]

// tag::main[]
int main()
{
    corosio::io_context ioc;

    echo_server server(ioc, 100);

    auto ec = server.bind(corosio::endpoint(8080));
    if (ec)
    {
        std::cerr << "Bind failed: " << ec.message() << "\n";
        return 1;
    }

    std::cout << "Echo server listening on port 8080\n";

    server.start();
    ioc.run();
}
// end::main[]
