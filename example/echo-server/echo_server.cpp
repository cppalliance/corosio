//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio.hpp>
#include <boost/corosio/acceptor.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/async_run.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

// Preallocated worker that handles one connection at a time
struct worker
{
    corosio::socket sock;
    std::string buf;
    bool in_use = false;

    explicit worker(corosio::io_context& ioc)
        : sock(ioc)
    {
        buf.reserve(4096);
    }

    worker(worker&&) = default;
    worker& operator=(worker&&) = default;
};

// Echo session coroutine for a single worker
// Reads data and echoes it back until the client disconnects
capy::task<void>
run_session(worker& w)
{
    w.in_use = true;

    for (;;)
    {
        w.buf.clear();
        w.buf.resize(4096);

        // Read some data
        auto [ec, n] = co_await w.sock.read_some(
            capy::mutable_buffer(w.buf.data(), w.buf.size()));

        if (ec || n == 0)
            break;

        w.buf.resize(n);

        // Echo it back
        auto [wec, wn] = co_await corosio::write(
            w.sock, capy::const_buffer(w.buf.data(), w.buf.size()));

        if (wec)
            break;
    }

    w.sock.close();
    w.in_use = false;
}

// Accept loop coroutine
// Accepts connections and assigns them to free workers
capy::task<void>
accept_loop(
    corosio::io_context& ioc,
    corosio::acceptor& acc,
    std::vector<worker>& workers)
{
    for (;;)
    {
        // Find a free worker
        worker* free_worker = nullptr;
        for (auto& w : workers)
        {
            if (!w.in_use)
            {
                free_worker = &w;
                break;
            }
        }

        if (!free_worker)
        {
            // All workers busy - this simple example just logs and continues
            // A production server might queue or reject connections
            std::cerr << "All workers busy, waiting...\n";
            // We need to accept anyway to not leave the client hanging
            // Create a temporary socket just to accept and close
            corosio::socket temp(ioc);
            auto [ec] = co_await acc.accept(temp);
            if (ec)
            {
                std::cerr << "Accept error: " << ec.message() << "\n";
                break;
            }
            temp.close();
            continue;
        }

        // Accept into the free worker's socket
        auto [ec] = co_await acc.accept(free_worker->sock);
        if (ec)
        {
            std::cerr << "Accept error: " << ec.message() << "\n";
            break;
        }

        // Spawn the session coroutine
        capy::async_run(ioc.get_executor())(run_session(*free_worker));
    }
}

int
main(int argc, char* argv[])
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

    // Preallocate workers
    std::vector<worker> workers;
    workers.reserve(max_workers);
    for (int i = 0; i < max_workers; ++i)
        workers.emplace_back(ioc);

    // Create acceptor and listen
    corosio::acceptor acc(ioc);
    acc.listen(corosio::endpoint(port));

    std::cout << "Echo server listening on port " << port
              << " with " << max_workers << " workers\n";

    // Start the accept loop
    capy::async_run(ioc.get_executor())(accept_loop(ioc, acc, workers));

    // Run the event loop
    ioc.run();

    return EXIT_SUCCESS;
}
