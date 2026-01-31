//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tcp_server.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <stop_token>

#include "test_suite.hpp"

namespace boost::corosio {
namespace {

// Simple test server for validating stop behavior
class test_server : public tcp_server
{
    io_context& ctx_;

    class test_worker : public worker_base
    {
        io_context& ctx_;
        corosio::socket sock_;

    public:
        explicit test_worker(io_context& ctx)
            : ctx_(ctx)
            , sock_(ctx)
        {
        }

        corosio::socket& socket() override
        {
            return sock_;
        }

        void run(launcher launch) override
        {
            launch(ctx_.get_executor(),
                [](corosio::socket* sock) -> capy::task<>
                {
                    // Echo one message and close
                    char buf[64];
                    auto [ec, n] = co_await sock->read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                    if(!ec)
                        (void)co_await sock->write_some(capy::const_buffer(buf, n));
                    sock->close();
                }(&sock_));
        }
    };

public:
    test_server(io_context& ctx)
        : tcp_server(ctx, ctx.get_executor())
        , ctx_(ctx)
    {
        wv_.reserve(4);
        for(int i = 0; i < 4; ++i)
            wv_.emplace<test_worker>(ctx_);
    }
};

} // namespace

struct tcp_server_test
{
    void
    testStopServer()
    {
        io_context ioc;
        test_server srv(ioc);

        // Bind to ephemeral port
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        std::stop_source stop_src;
        std::atomic<bool> client_done{false};

        // Start server with stop token
        srv.start(stop_src.get_token());

        // Client task: request stop after brief delay
        auto client_task = [](
            io_context* ioc,
            std::stop_source* stop_src,
            std::atomic<bool>* client_done) -> capy::task<>
        {
            // Brief delay to ensure server accept loop is running
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();

            // Request stop - server should exit accept loop
            stop_src->request_stop();

            client_done->store(true);
        }(&ioc, &stop_src, &client_done);

        capy::run_async(ioc.get_executor())(std::move(client_task));

        // Run until all work completes
        ioc.run();

        BOOST_TEST(client_done.load());
    }

    void
    testStopWithActiveConnection()
    {
        io_context ioc;

        // Find an available port
        acceptor acc(ioc);
        std::uint16_t port = 0;
        for(int attempt = 0; attempt < 20; ++attempt)
        {
            port = static_cast<std::uint16_t>(49152 + (attempt * 7) % 16383);
            try
            {
                acc.listen(endpoint(ipv4_address::loopback(), port));
                break;
            }
            catch(std::system_error const&)
            {
                acc.close();
                acc = acceptor(ioc);
            }
        }
        acc.close();

        // Create server and bind to found port
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(!ec);

        std::stop_source stop_src;
        std::atomic<bool> connection_handled{false};
        std::atomic<bool> stop_requested{false};

        srv.start(stop_src.get_token());

        // Client connects, exchanges data, then triggers stop
        auto client_task = [](
            io_context* ioc,
            std::uint16_t port,
            std::stop_source* stop_src,
            std::atomic<bool>* connection_handled,
            std::atomic<bool>* stop_requested) -> capy::task<>
        {
            socket client(*ioc);
            client.open();

            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if(connect_ec)
            {
                co_return;
            }

            // Send data
            auto [write_ec, written] = co_await client.write_some(
                capy::const_buffer("hello", 5));
            BOOST_TEST(!write_ec);

            // Read echo
            char buf[64];
            auto [read_ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!read_ec);
            BOOST_TEST_EQ(n, 5u);

            connection_handled->store(true);
            client.close();

            // Now request stop
            stop_src->request_stop();
            stop_requested->store(true);
        }(&ioc, port, &stop_src, &connection_handled, &stop_requested);

        capy::run_async(ioc.get_executor())(std::move(client_task));

        ioc.run();

        BOOST_TEST(connection_handled.load());
        BOOST_TEST(stop_requested.load());
    }

    void
    run()
    {
        testStopServer();
        testStopWithActiveConnection();
    }
};

TEST_SUITE(tcp_server_test, "boost.corosio.tcp_server");

} // namespace boost::corosio
