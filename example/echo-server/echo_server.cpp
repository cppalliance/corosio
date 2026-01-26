//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_server.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

class echo_server : public corosio::tcp_server
{
    class worker : public worker_base
    {
        corosio::io_context& ctx_;
        corosio::socket sock_;
        std::string buf_;

    public:
        explicit worker(corosio::io_context& ctx)
            : ctx_(ctx)
            , sock_(ctx)
        {
            buf_.reserve(4096);
        }

        corosio::socket& socket() override
        {
            return sock_;
        }

        void run(launcher launch) override
        {
            launch(ctx_.get_executor(), do_session());
        }

        capy::task<> do_session()
        {
            for (;;)
            {
                buf_.resize(4096);

                // Read some data
                auto [ec, n] = co_await sock_.read_some(
                    capy::mutable_buffer(buf_.data(), buf_.size()));

                if (ec || n == 0)
                    break;

                buf_.resize(n);

                // Echo it back
                auto [wec, wn] = co_await capy::write(
                    sock_, capy::const_buffer(buf_.data(), buf_.size()));

                if (wec)
                    break;
            }

            sock_.close();
        }
    };

public:
    echo_server(corosio::io_context& ctx, int max_workers)
        : tcp_server(ctx, ctx.get_executor())
    {
        wv_.reserve(max_workers);
        for (int i = 0; i < max_workers; ++i)
            wv_.emplace<worker>(ctx);
    }
};

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr <<
            "Usage: echo_server <port> <max-workers>\n"
            "Example:\n"
            "    echo_server 8080 10\n";
        return EXIT_FAILURE;
    }

    // Parse port
    int port_int = std::atoi(argv[1]);
    if (port_int <= 0 || port_int > 65535)
    {
        std::cerr << "Invalid port: " << argv[1] << "\n";
        return EXIT_FAILURE;
    }
    auto port = static_cast<std::uint16_t>(port_int);

    // Parse max workers
    int max_workers = std::atoi(argv[2]);
    if (max_workers <= 0)
    {
        std::cerr << "Invalid max-workers: " << argv[2] << "\n";
        return EXIT_FAILURE;
    }

    // Create I/O context
    corosio::io_context ioc;

    // Create server with worker pool
    echo_server server(ioc, max_workers);

    // Bind to port
    auto ec = server.bind(corosio::endpoint(port));
    if (ec)
    {
        std::cerr << "Bind failed: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Echo server listening on port " << port
              << " with " << max_workers << " workers\n";

    // Start accepting connections
    server.start();

    // Run the event loop
    ioc.run();

    return EXIT_SUCCESS;
}
