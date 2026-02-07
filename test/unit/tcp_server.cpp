//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
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

#include "test_suite.hpp"

namespace boost::corosio {
namespace {

class test_worker : public tcp_server::worker_base
{
    io_context& ctx_;
    corosio::tcp_socket sock_;

public:
    explicit test_worker(io_context& ctx)
        : ctx_(ctx)
        , sock_(ctx)
    {
    }

    corosio::tcp_socket& socket() override
    {
        return sock_;
    }

    void run(tcp_server::launcher launch) override
    {
        launch(ctx_.get_executor(),
            [](corosio::tcp_socket* sock) -> capy::task<>
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

inline auto
make_test_workers(io_context& ctx, int n)
{
    std::vector<std::unique_ptr<tcp_server::worker_base>> v;
    v.reserve(n);
    for(int i = 0; i < n; ++i)
        v.push_back(std::make_unique<test_worker>(ctx));
    return v;
}

// Simple test server for validating stop behavior
class test_server : public tcp_server
{
public:
    test_server(io_context& ctx)
        : tcp_server(ctx, ctx.get_executor())
    {
        set_workers(make_test_workers(ctx, 4));
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

        std::atomic<bool> client_done{false};

        // Start server
        srv.start();

        // Client task: request stop after brief delay
        auto client_task = [](
            io_context* ioc,
            test_server* srv,
            std::atomic<bool>* client_done) -> capy::task<>
        {
            // Brief delay to ensure server accept loop is running
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();

            // Request stop - server should exit accept loop
            srv->stop();

            client_done->store(true);
        }(&ioc, &srv, &client_done);

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
        tcp_acceptor acc(ioc);
        std::uint16_t port = 0;
        for(int attempt = 0; attempt < 20; ++attempt)
        {
            port = static_cast<std::uint16_t>(49152 + (attempt * 7) % 16383);
            if (!acc.listen(endpoint(ipv4_address::loopback(), port)))
                break;
            acc.close();
            acc = tcp_acceptor(ioc);
        }
        acc.close();

        // Create server and bind to found port
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(!ec);

        std::atomic<bool> connection_handled{false};
        std::atomic<bool> stop_requested{false};

        srv.start();

        // Client connects, exchanges data, then triggers stop
        auto client_task = [](
            io_context* ioc,
            std::uint16_t port,
            test_server* srv,
            std::atomic<bool>* connection_handled,
            std::atomic<bool>* stop_requested) -> capy::task<>
        {
            tcp_socket client(*ioc);
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
            srv->stop();
            stop_requested->store(true);
        }(&ioc, port, &srv, &connection_handled, &stop_requested);

        capy::run_async(ioc.get_executor())(std::move(client_task));

        ioc.run();

        BOOST_TEST(connection_handled.load());
        BOOST_TEST(stop_requested.load());
    }

    void
    testStartIdempotent()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Calling start() twice should be safe
        srv.start();
        srv.start();  // Second call should be no-op

        auto task = [](
            io_context* ioc,
            test_server* srv) -> capy::task<>
        {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();
            srv->stop();
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void
    testStopIdempotent()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        srv.start();

        auto task = [](
            io_context* ioc,
            test_server* srv) -> capy::task<>
        {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();

            // Calling stop() twice should be safe
            srv->stop();
            srv->stop();  // Second call should be no-op
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void
    testStopWithoutStart()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Calling stop() without start() should be safe (no-op)
        srv.stop();
    }

    void
    testRestart()
    {
        // Test the "stop the world" pattern:
        // start -> run -> stop -> run (drain) -> join -> restart

        io_context ioc;

        // Find an available port
        tcp_acceptor acc(ioc);
        std::uint16_t port = 0;
        for(int attempt = 0; attempt < 20; ++attempt)
        {
            port = static_cast<std::uint16_t>(49152 + (attempt * 7) % 16383);
            if (!acc.listen(endpoint(ipv4_address::loopback(), port)))
                break;
            acc.close();
            acc = tcp_acceptor(ioc);
        }
        acc.close();

        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(!ec);

        int connections_handled = 0;

        // First session
        srv.start();

        auto task1 = [](
            io_context* ioc,
            std::uint16_t port,
            int* count) -> capy::task<>
        {
            tcp_socket client(*ioc);
            client.open();
            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if(!connect_ec)
            {
                auto [write_ec, written] = co_await client.write_some(
                    capy::const_buffer("hello", 5));
                if(!write_ec)
                {
                    char buf[64];
                    auto [read_ec, n] = co_await client.read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                    if(!read_ec)
                        ++(*count);
                }
            }
            client.close();
        }(&ioc, port, &connections_handled);

        auto stop_task1 = [](
            io_context* ioc,
            test_server* srv) -> capy::task<>
        {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();
            srv->stop();
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task1));
        capy::run_async(ioc.get_executor())(std::move(stop_task1));
        ioc.run();  // Runs until stopped and drained
        srv.join(); // Wait for accept loops

        BOOST_TEST_EQ(connections_handled, 1);

        // Restart for second session
        ioc.restart();
        srv.start();

        auto task2 = [](
            io_context* ioc,
            std::uint16_t port,
            int* count) -> capy::task<>
        {
            tcp_socket client(*ioc);
            client.open();
            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if(!connect_ec)
            {
                auto [write_ec, written] = co_await client.write_some(
                    capy::const_buffer("world", 5));
                if(!write_ec)
                {
                    char buf[64];
                    auto [read_ec, n] = co_await client.read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                    if(!read_ec)
                        ++(*count);
                }
            }
            client.close();
        }(&ioc, port, &connections_handled);

        auto stop_task2 = [](
            io_context* ioc,
            test_server* srv) -> capy::task<>
        {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();
            srv->stop();
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task2));
        capy::run_async(ioc.get_executor())(std::move(stop_task2));
        ioc.run();
        srv.join();

        BOOST_TEST_EQ(connections_handled, 2);
    }

    void
    testStartWithoutJoinThrows()
    {
        // Deterministic test: start() throws if previous session not joined
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // First start - launches accept loops (active_accepts_ > 0)
        srv.start();

        // Stop but don't drain or join
        srv.stop();

        // Second start should throw because active_accepts_ != 0
        // (accept loops haven't had a chance to run their completion handlers)
        bool threw = false;
        try
        {
            srv.start();
        }
        catch(std::logic_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);

        // Now properly drain and join
        ioc.run();
        srv.join();

        // After join, start should work
        ioc.restart();  // Required before running again
        srv.start();
        srv.stop();
        ioc.run();
        srv.join();
    }

    void
    testListenErrorCode()
    {
        io_context ioc;

        // Test success case
        tcp_acceptor acc1(ioc);
        auto ec1 = acc1.listen(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        BOOST_TEST(acc1.is_open());
        auto port = acc1.local_endpoint().port();
        BOOST_TEST(port != 0);

        // Test with explicit backlog
        tcp_acceptor acc2(ioc);
        auto ec2 = acc2.listen(endpoint(ipv4_address::loopback(), 0), 64);
        BOOST_TEST(!ec2);
        BOOST_TEST(acc2.is_open());
        BOOST_TEST(acc2.local_endpoint().port() != 0);
    }

    void
    testBindSuccess()
    {
        io_context ioc;

        // Test that tcp_server::bind returns no error and doesn't throw
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
    }

    void
    testListenErrorNonLocalAddress()
    {
        io_context ioc;

        // Binding to a non-local IP address should fail with
        // "can't assign requested address" (EADDRNOTAVAIL) on all platforms.
        // 192.0.2.1 is from TEST-NET-1 (RFC 5737), reserved for documentation
        // and never assigned to real interfaces.
        tcp_acceptor acc(ioc);
        auto ec = acc.listen(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
        BOOST_TEST(!acc.is_open());
    }

    void
    testBindErrorNonLocalAddress()
    {
        io_context ioc;

        // tcp_server::bind should return an error for non-local address
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
    }

    void
    testListenOnOpenAcceptor()
    {
        io_context ioc;
        tcp_acceptor acc(ioc);

        // First listen
        auto ec1 = acc.listen(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        BOOST_TEST(acc.is_open());

        // Re-listen should close and reopen
        auto ec2 = acc.listen(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec2);
        BOOST_TEST(acc.is_open());
    }

    void
    run()
    {
        testStopServer();
        testStopWithActiveConnection();
        testStartIdempotent();
        testStopIdempotent();
        testStopWithoutStart();
        testRestart();
        testStartWithoutJoinThrows();
        testListenErrorCode();
        testBindSuccess();
        testListenErrorNonLocalAddress();
        testBindErrorNonLocalAddress();
        testListenOnOpenAcceptor();
    }
};

TEST_SUITE(tcp_server_test, "boost.corosio.tcp_server");

} // namespace boost::corosio
