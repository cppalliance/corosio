//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Complete example shown in pages/4.guide/4k.tcp-server.adoc. Compiled
// and linked only: main() binds a port and runs forever, and the
// runtime behavior is covered by test/doc/run_examples.py against the
// real echo-server example.

// tag::full[]
#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>
#include <iostream>
#include <memory>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

class echo_worker : public corosio::tcp_server::worker_base
{
    corosio::io_context& ctx_;
    corosio::tcp_socket sock_;
    std::string buf;

public:
    explicit echo_worker(corosio::io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {
        buf.reserve(4096);
    }

    corosio::tcp_socket& socket() override { return sock_; }

    void run(corosio::tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(), do_session());
    }

    capy::task<void> do_session()
    {
        for (;;)
        {
            buf.resize(4096);
            auto [ec, n] = co_await sock_.read_some(
                capy::mutable_buffer(buf.data(), buf.size()));

            if (ec || n == 0)
                break;

            buf.resize(n);
            auto [wec, wn] = co_await capy::write(
                sock_, capy::const_buffer(buf.data(), buf.size()));

            if (wec)
                break;
        }

        sock_.close();
    }
};

auto make_echo_workers(corosio::io_context& ctx, int n)
{
    std::vector<std::unique_ptr<corosio::tcp_server::worker_base>> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<echo_worker>(ctx));
    return v;
}

class echo_server : public corosio::tcp_server
{
public:
    echo_server(corosio::io_context& ctx, int max_workers)
        : tcp_server(ctx, ctx.get_executor())
    {
        set_workers(make_echo_workers(ctx, max_workers));
    }
};

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
// end::full[]
