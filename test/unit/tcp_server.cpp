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
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/buffers.hpp>
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
    explicit test_worker(io_context& ctx) : ctx_(ctx), sock_(ctx) {}

    corosio::tcp_socket& socket() override
    {
        return sock_;
    }

    void run(tcp_server::launcher launch) override
    {
        launch(
            ctx_.get_executor(), [](corosio::tcp_socket* sock) -> capy::task<> {
                // Echo one message and close
                char buf[64];
                auto [ec, n] = co_await sock->read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
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
    for (int i = 0; i < n; ++i)
        v.push_back(std::make_unique<test_worker>(ctx));
    return v;
}

// Simple test server for validating stop behavior
class test_server : public tcp_server
{
public:
    test_server(io_context& ctx) : tcp_server(ctx, ctx.get_executor())
    {
        set_workers(make_test_workers(ctx, 4));
    }
};

} // namespace

struct tcp_server_test
{
    void testStopServer()
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
        auto client_task = [](io_context* ioc, test_server* srv,
                              std::atomic<bool>* client_done) -> capy::task<> {
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

    void testStopWithActiveConnection()
    {
        io_context ioc;

        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        std::atomic<bool> connection_handled{false};
        std::atomic<bool> stop_requested{false};

        srv.start();

        // Client connects, exchanges data, then triggers stop
        auto client_task =
            [](io_context* ioc, std::uint16_t port, test_server* srv,
               std::atomic<bool>* connection_handled,
               std::atomic<bool>* stop_requested) -> capy::task<> {
            tcp_socket client(*ioc);
            client.open();

            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if (connect_ec)
            {
                co_return;
            }

            // Send data
            auto [write_ec, written] =
                co_await client.write_some(capy::const_buffer("hello", 5));
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

    void testStartIdempotent()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Calling start() twice should be safe
        srv.start();
        srv.start(); // Second call should be no-op

        auto task = [](io_context* ioc, test_server* srv) -> capy::task<> {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();
            srv->stop();
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void testStopIdempotent()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        srv.start();

        auto task = [](io_context* ioc, test_server* srv) -> capy::task<> {
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();

            // Calling stop() twice should be safe
            srv->stop();
            srv->stop(); // Second call should be no-op
        }(&ioc, &srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void testStopWithoutStart()
    {
        io_context ioc;
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Calling stop() without start() should be safe (no-op)
        srv.stop();
    }

    void testRestart()
    {
        // Test the "stop the world" pattern:
        // start -> run -> stop -> run (drain) -> join -> restart

        io_context ioc;

        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        int connections_handled = 0;

        // First session
        srv.start();

        auto task1 = [](io_context* ioc, std::uint16_t port,
                        int* count) -> capy::task<> {
            tcp_socket client(*ioc);
            client.open();
            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if (!connect_ec)
            {
                auto [write_ec, written] =
                    co_await client.write_some(capy::const_buffer("hello", 5));
                if (!write_ec)
                {
                    char buf[64];
                    auto [read_ec, n] = co_await client.read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                    if (!read_ec)
                        ++(*count);
                }
            }
            client.close();
        }(&ioc, port, &connections_handled);

        auto stop_task1 = [](io_context* ioc,
                             test_server* srv) -> capy::task<> {
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

        auto task2 = [](io_context* ioc, std::uint16_t port,
                        int* count) -> capy::task<> {
            tcp_socket client(*ioc);
            client.open();
            auto [connect_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            if (!connect_ec)
            {
                auto [write_ec, written] =
                    co_await client.write_some(capy::const_buffer("world", 5));
                if (!write_ec)
                {
                    char buf[64];
                    auto [read_ec, n] = co_await client.read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                    if (!read_ec)
                        ++(*count);
                }
            }
            client.close();
        }(&ioc, port, &connections_handled);

        auto stop_task2 = [](io_context* ioc,
                             test_server* srv) -> capy::task<> {
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

    void testStartWithoutJoinThrows()
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
        catch (std::logic_error const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);

        // Now properly drain and join
        ioc.run();
        srv.join();

        // After join, start should work
        ioc.restart(); // Required before running again
        srv.start();
        srv.stop();
        ioc.run();
        srv.join();
    }

    void testListenErrorCode()
    {
        io_context ioc;

        // Test success case
        tcp_acceptor acc1(ioc);
        acc1.open();
        acc1.set_option(socket_option::reuse_address(true));
        auto ec1 = acc1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        ec1 = acc1.listen();
        BOOST_TEST(!ec1);
        BOOST_TEST(acc1.is_open());
        auto port = acc1.local_endpoint().port();
        BOOST_TEST(port != 0);

        // Test with explicit backlog
        tcp_acceptor acc2(ioc);
        acc2.open();
        acc2.set_option(socket_option::reuse_address(true));
        auto ec2 = acc2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec2);
        ec2 = acc2.listen(64);
        BOOST_TEST(!ec2);
        BOOST_TEST(acc2.is_open());
        BOOST_TEST(acc2.local_endpoint().port() != 0);
    }

    void testBindSuccess()
    {
        io_context ioc;

        // Test that tcp_server::bind returns no error and doesn't throw
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
    }

    void testBindErrorNonLocalAcceptor()
    {
        io_context ioc;

        // Binding to a non-local IP address should fail with
        // "can't assign requested address" (EADDRNOTAVAIL) on all platforms.
        // 192.0.2.1 is from TEST-NET-1 (RFC 5737), reserved for documentation
        // and never assigned to real interfaces.
        tcp_acceptor acc(ioc);
        acc.open();
        auto ec = acc.bind(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
        acc.close();
    }

    void testBindErrorNonLocalAddress()
    {
        io_context ioc;

        // tcp_server::bind should return an error for non-local address
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
    }

    void testRelistenAfterClose()
    {
        io_context ioc;
        tcp_acceptor acc(ioc);

        // First listen
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec1 = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        ec1 = acc.listen();
        BOOST_TEST(!ec1);
        BOOST_TEST(acc.is_open());

        // Close and re-listen
        acc.close();
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec2 = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec2);
        ec2 = acc.listen();
        BOOST_TEST(!ec2);
        BOOST_TEST(acc.is_open());
    }

    void testMoveConstruct()
    {
        // Verify the noexcept move constructor leaves the source empty
        // and the destination with the original state.
        io_context ioc;
        test_server src(ioc);
        auto ec = src.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = src.local_endpoint().port();
        BOOST_TEST(port != 0);

        test_server dst(std::move(src));

        // Destination retains the bound port; source local_endpoint
        // becomes default-constructed (impl moved out).
        BOOST_TEST_EQ(dst.local_endpoint().port(), port);
    }

    void testMoveAssign()
    {
        // Move-assign discards the existing impl_ and adopts the source.
        io_context ioc;
        test_server a(ioc);
        test_server b(ioc);

        auto ec = a.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port_a = a.local_endpoint().port();

        ec = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        b = std::move(a);
        BOOST_TEST_EQ(b.local_endpoint().port(), port_a);
    }

    void testLauncherDtorReturnsWorker()
    {
        // worker_base::run() never invokes the launcher; the launcher
        // destructor must return the worker to the idle pool via
        // push_sync. After stop() the accept loop completes cleanly.
        io_context ioc;

        class no_launch_worker : public tcp_server::worker_base
        {
            corosio::tcp_socket sock_;

        public:
            std::atomic<int>* run_count = nullptr;

            no_launch_worker(io_context& ctx, std::atomic<int>* c)
                : sock_(ctx), run_count(c)
            {
            }

            corosio::tcp_socket& socket() override { return sock_; }

            void run(tcp_server::launcher) override
            {
                // Drop launcher without invoking it. Its destructor
                // must push the worker back to the idle pool.
                run_count->fetch_add(1);
                sock_.close();
            }
        };

        std::atomic<int> run_count{0};

        class drop_server : public tcp_server
        {
        public:
            drop_server(io_context& ctx, std::atomic<int>* c)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<no_launch_worker>(ctx, c));
                set_workers(std::move(v));
            }
        };

        drop_server srv(ioc, &run_count);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        srv.start();

        // Two clients exercise the worker pool: the worker is dropped
        // back to idle by ~launcher() between connections.
        auto driver = [](io_context* ioc, std::uint16_t port,
                         drop_server* srv) -> capy::task<> {
            for (int i = 0; i < 2; ++i)
            {
                tcp_socket client(*ioc);
                client.open();
                auto [cec] = co_await client.connect(
                    endpoint(ipv4_address::loopback(), port));
                (void)cec;
                client.close();
                timer t(*ioc);
                t.expires_after(std::chrono::milliseconds(20));
                (void)co_await t.wait();
            }
            srv->stop();
        }(&ioc, port, &srv);

        capy::run_async(ioc.get_executor())(std::move(driver));
        ioc.run();
        srv.join();

        BOOST_TEST(run_count.load() >= 1);
    }

    void testMultipleActiveConnections()
    {
        // Multiple concurrent connections exercise the active list's
        // doubly-linked-list bookkeeping (push_back/remove with prev/next).
        io_context ioc;

        // Worker that holds the connection open until told to release.
        class slow_worker : public tcp_server::worker_base
        {
            io_context& ctx_;
            corosio::tcp_socket sock_;

        public:
            explicit slow_worker(io_context& ctx) : ctx_(ctx), sock_(ctx) {}

            corosio::tcp_socket& socket() override { return sock_; }

            void run(tcp_server::launcher launch) override
            {
                launch(
                    ctx_.get_executor(),
                    [](io_context* ctx,
                       corosio::tcp_socket* s) -> capy::task<> {
                        // Block on read until the client disconnects.
                        char buf[64];
                        auto [ec, n] = co_await s->read_some(
                            capy::mutable_buffer(buf, sizeof(buf)));
                        (void)ec;
                        (void)n;
                        s->close();
                        (void)ctx;
                    }(&ctx_, &sock_));
            }
        };

        class multi_server : public tcp_server
        {
        public:
            explicit multi_server(io_context& ctx)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.reserve(4);
                for (int i = 0; i < 4; ++i)
                    v.push_back(std::make_unique<slow_worker>(ctx));
                set_workers(std::move(v));
            }
        };

        multi_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        srv.start();

        std::atomic<int> connected{0};

        // Open 3 simultaneous connections to push three workers into
        // the active list, then disconnect them in reverse order so
        // both endpoints (head/tail/middle) of active_remove are taken.
        auto driver = [](io_context* ioc, std::uint16_t port,
                         std::atomic<int>* connected,
                         multi_server* srv) -> capy::task<> {
            tcp_socket c1(*ioc), c2(*ioc), c3(*ioc);
            c1.open(); c2.open(); c3.open();

            auto [e1] = co_await c1.connect(
                endpoint(ipv4_address::loopback(), port));
            if (!e1) connected->fetch_add(1);
            auto [e2] = co_await c2.connect(
                endpoint(ipv4_address::loopback(), port));
            if (!e2) connected->fetch_add(1);
            auto [e3] = co_await c3.connect(
                endpoint(ipv4_address::loopback(), port));
            if (!e3) connected->fetch_add(1);

            // Give the server time to register the connections.
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();

            // Disconnect middle first, then tail, then head:
            // exercises remove from each list position.
            c2.close();
            timer t1(*ioc); t1.expires_after(std::chrono::milliseconds(20));
            (void)co_await t1.wait();
            c3.close();
            timer t2(*ioc); t2.expires_after(std::chrono::milliseconds(20));
            (void)co_await t2.wait();
            c1.close();
            timer t3(*ioc); t3.expires_after(std::chrono::milliseconds(20));
            (void)co_await t3.wait();

            srv->stop();
        }(&ioc, port, &connected, &srv);

        capy::run_async(ioc.get_executor())(std::move(driver));
        ioc.run();
        srv.join();

        BOOST_TEST(connected.load() >= 1);
    }

    void testLauncherDoubleInvokeThrows()
    {
        // The second invocation of a launcher must throw logic_error.
        // We accept the connection but invoke launch twice inside run().
        io_context ioc;

        class throwing_worker : public tcp_server::worker_base
        {
            io_context& ctx_;
            corosio::tcp_socket sock_;

        public:
            std::atomic<bool>* threw = nullptr;

            throwing_worker(io_context& ctx, std::atomic<bool>* t)
                : ctx_(ctx), sock_(ctx), threw(t)
            {
            }

            corosio::tcp_socket& socket() override { return sock_; }

            void run(tcp_server::launcher launch) override
            {
                launch(
                    ctx_.get_executor(),
                    [](corosio::tcp_socket* s) -> capy::task<> {
                        s->close();
                        co_return;
                    }(&sock_));

                // Second invocation must throw std::logic_error.
                try
                {
                    launch(
                        ctx_.get_executor(),
                        [](corosio::tcp_socket*) -> capy::task<> {
                            co_return;
                        }(&sock_));
                }
                catch (std::logic_error const&)
                {
                    threw->store(true);
                }
            }
        };

        std::atomic<bool> launcher_threw{false};

        class one_worker_server : public tcp_server
        {
        public:
            one_worker_server(io_context& ctx, std::atomic<bool>* t)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<throwing_worker>(ctx, t));
                set_workers(std::move(v));
            }
        };

        one_worker_server srv(ioc, &launcher_threw);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        srv.start();

        auto client_task = [](io_context* ioc, std::uint16_t port,
                              one_worker_server* srv) -> capy::task<> {
            tcp_socket client(*ioc);
            client.open();
            auto [cec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            (void)cec;
            client.close();

            // Give server time to handle the connection, then stop.
            timer t(*ioc);
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();
            srv->stop();
        }(&ioc, port, &srv);

        capy::run_async(ioc.get_executor())(std::move(client_task));
        ioc.run();
        srv.join();

        BOOST_TEST(launcher_threw.load());
    }

    void testLocalEndpointOutOfRange()
    {
        // Index past the bound-ports list returns a default endpoint.
        io_context ioc;
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        BOOST_TEST(srv.local_endpoint(0).port() != 0);
        BOOST_TEST(srv.local_endpoint(99) == endpoint{});
    }

    void testWaitersWakeOnWorkerReturn()
    {
        // With one worker handling two sequential connections, the
        // second accept loop iteration must wait for the worker to
        // return (exercises pop_awaitable suspend / push_awaitable wake).
        io_context ioc;

        class one_worker_server : public tcp_server
        {
        public:
            explicit one_worker_server(io_context& ctx)
                : tcp_server(ctx, ctx.get_executor())
            {
                set_workers(make_test_workers(ctx, 1));
            }
        };

        one_worker_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        std::atomic<int> echoed{0};

        srv.start();

        auto driver = [](io_context* ioc, std::uint16_t port,
                         std::atomic<int>* echoed,
                         one_worker_server* srv) -> capy::task<> {
            for (int i = 0; i < 2; ++i)
            {
                tcp_socket client(*ioc);
                client.open();
                auto [cec] = co_await client.connect(
                    endpoint(ipv4_address::loopback(), port));
                if (cec)
                    continue;
                char msg[] = "ab";
                auto [wec, wn] =
                    co_await client.write_some(capy::const_buffer(msg, 2));
                if (wec)
                    continue;
                char buf[4];
                auto [rec, rn] = co_await client.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                if (!rec && rn == 2)
                    echoed->fetch_add(1);
                client.close();
            }
            srv->stop();
        }(&ioc, port, &echoed, &srv);

        capy::run_async(ioc.get_executor())(std::move(driver));
        ioc.run();
        srv.join();

        BOOST_TEST_EQ(echoed.load(), 2);
    }

    void run()
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
        testBindErrorNonLocalAcceptor();
        testBindErrorNonLocalAddress();
        testRelistenAfterClose();
        testMoveConstruct();
        testMoveAssign();
        testLocalEndpointOutOfRange();
        testWaitersWakeOnWorkerReturn();
        testLauncherDoubleInvokeThrows();
        testLauncherDtorReturnsWorker();
        testMultipleActiveConnections();
    }
};

TEST_SUITE(tcp_server_test, "boost.corosio.tcp_server");

} // namespace boost::corosio
