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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/delay.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/abandon.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <chrono>
#include <vector>
#include <thread>

#include "context.hpp"
#include "must.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

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
                std::ignore =
                    co_await sock->write_some(capy::const_buffer(buf, n));
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

// Counts the blocks it hands out that are still live.
struct counting_resource : std::pmr::memory_resource
{
    int live = 0;

    void* do_allocate(std::size_t n, std::size_t align) override
    {
        ++live;
        return std::pmr::new_delete_resource()->allocate(n, align);
    }

    void do_deallocate(void* p, std::size_t n, std::size_t align) override
    {
        --live;
        std::pmr::new_delete_resource()->deallocate(p, n, align);
    }

    bool do_is_equal(std::pmr::memory_resource const& o) const noexcept override
    {
        return this == &o;
    }
};

template<auto Backend>
struct tcp_server_test
{
    void testStopServer()
    {
        io_context ioc(Backend);
        test_server srv(ioc);

        // Bind to ephemeral port
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        std::atomic<bool> client_done{false};

        // Start server
        srv.start();

        // Client task: request stop after brief delay
        auto client_task = [](test_server* srv,
                              std::atomic<bool>* client_done) -> capy::task<> {
            // Brief delay to ensure server accept loop is running
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));

            // Request stop - server should exit accept loop
            srv->stop();

            client_done->store(true);
        }(&srv, &client_done);

        capy::run_async(ioc.get_executor())(std::move(client_task));

        // Run until all work completes
        ioc.run();

        BOOST_TEST(client_done.load());
    }

    void testStopWithActiveConnection()
    {
        io_context ioc(Backend);

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
            BOOST_TEST(!client.open());

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
        io_context ioc(Backend);
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Calling start() twice should be safe
        srv.start();
        srv.start(); // Second call should be no-op

        auto task = [](test_server* srv) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));
            srv->stop();
        }(&srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void testStopIdempotent()
    {
        io_context ioc(Backend);
        test_server srv(ioc);

        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        srv.start();

        auto task = [](test_server* srv) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(10));

            // Calling stop() twice should be safe
            srv->stop();
            srv->stop(); // Second call should be no-op
        }(&srv);

        capy::run_async(ioc.get_executor())(std::move(task));
        ioc.run();
    }

    void testStopWithoutStart()
    {
        io_context ioc(Backend);
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

        io_context ioc(Backend);

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
            BOOST_TEST(!client.open());
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

        auto stop_task1 = [](test_server* srv) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(50));
            srv->stop();
        }(&srv);

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
            BOOST_TEST(!client.open());
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

        auto stop_task2 = [](test_server* srv) -> capy::task<> {
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(50));
            srv->stop();
        }(&srv);

        capy::run_async(ioc.get_executor())(std::move(task2));
        capy::run_async(ioc.get_executor())(std::move(stop_task2));
        ioc.run();
        srv.join();

        BOOST_TEST_EQ(connections_handled, 2);
    }

    void testStartWithoutJoinThrows()
    {
        // Deterministic test: start() throws if previous session not joined
        io_context ioc(Backend);
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
        io_context ioc(Backend);

        // Test success case
        tcp_acceptor acc1(ioc);
        BOOST_TEST(!acc1.open());
        test::must_ok(acc1.set_option(socket_option::reuse_address(true)));
        auto ec1 = acc1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        ec1 = acc1.listen();
        BOOST_TEST(!ec1);
        BOOST_TEST(acc1.is_open());
        auto port = acc1.local_endpoint().port();
        BOOST_TEST(port != 0);

        // Test with explicit backlog
        tcp_acceptor acc2(ioc);
        BOOST_TEST(!acc2.open());
        test::must_ok(acc2.set_option(socket_option::reuse_address(true)));
        auto ec2 = acc2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec2);
        ec2 = acc2.listen(64);
        BOOST_TEST(!ec2);
        BOOST_TEST(acc2.is_open());
        BOOST_TEST(acc2.local_endpoint().port() != 0);
    }

    void testBindSuccess()
    {
        io_context ioc(Backend);

        // Test that tcp_server::bind returns no error and doesn't throw
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
    }

    void testBindErrorNonLocalAcceptor()
    {
        io_context ioc(Backend);

        // Binding to a non-local IP address should fail with
        // "can't assign requested address" (EADDRNOTAVAIL) on all platforms.
        // 192.0.2.1 is from TEST-NET-1 (RFC 5737), reserved for documentation
        // and never assigned to real interfaces.
        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto ec = acc.bind(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
        acc.close();
    }

    void testBindCollision()
    {
        io_context ioc(Backend);

        tcp_acceptor first(ioc);
        test::must_ok(first.open());
        test::must_ok(first.bind(endpoint(ipv4_address::loopback(), 0)));
        test::must_ok(first.listen());
        auto const port = first.local_endpoint().port();

        // Holds on every platform, including those where SO_REUSEADDR
        // would let a second listener share the port.
        test_server srv(ioc);
        BOOST_TEST(
            srv.bind(endpoint(ipv4_address::loopback(), port)) ==
            std::errc::address_in_use);
    }

    void testBindErrorNonLocalAddress()
    {
        io_context ioc(Backend);

        // tcp_server::bind should return an error for non-local address
        test_server srv(ioc);
        auto ec = srv.bind(endpoint(ipv4_address({192, 0, 2, 1}), 0));
        BOOST_TEST(ec);
    }

    void testRelistenAfterClose()
    {
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // First listen
        BOOST_TEST(!acc.open());
        test::must_ok(acc.set_option(socket_option::reuse_address(true)));
        auto ec1 = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        ec1 = acc.listen();
        BOOST_TEST(!ec1);
        BOOST_TEST(acc.is_open());

        // Close and re-listen
        acc.close();
        BOOST_TEST(!acc.open());
        test::must_ok(acc.set_option(socket_option::reuse_address(true)));
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
        io_context ioc(Backend);
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
        io_context ioc(Backend);
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
        io_context ioc(Backend);

        class no_launch_worker : public tcp_server::worker_base
        {
            corosio::tcp_socket sock_;

        public:
            std::atomic<int>* run_count = nullptr;

            no_launch_worker(io_context& ctx, std::atomic<int>* c)
                : sock_(ctx)
                , run_count(c)
            {
            }

            corosio::tcp_socket& socket() override
            {
                return sock_;
            }

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
                BOOST_TEST(!client.open());
                [[maybe_unused]] auto [cec] = co_await client.connect(
                    endpoint(ipv4_address::loopback(), port));
                client.close();
                std::ignore =
                    co_await corosio::delay(std::chrono::milliseconds(20));
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
        io_context ioc(Backend);

        // Worker that holds the connection open until told to release.
        class slow_worker : public tcp_server::worker_base
        {
            io_context& ctx_;
            corosio::tcp_socket sock_;

        public:
            explicit slow_worker(io_context& ctx) : ctx_(ctx), sock_(ctx) {}

            corosio::tcp_socket& socket() override
            {
                return sock_;
            }

            void run(tcp_server::launcher launch) override
            {
                launch(
                    ctx_.get_executor(),
                    []([[maybe_unused]] io_context* ctx,
                       corosio::tcp_socket* s) -> capy::task<> {
                        // Block on read until the client disconnects.
                        char buf[64];
                        [[maybe_unused]] auto [ec, n] = co_await s->read_some(
                            capy::mutable_buffer(buf, sizeof(buf)));
                        s->close();
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

            auto [e1] =
                co_await c1.connect(endpoint(ipv4_address::loopback(), port));
            if (!e1)
                connected->fetch_add(1);
            auto [e2] =
                co_await c2.connect(endpoint(ipv4_address::loopback(), port));
            if (!e2)
                connected->fetch_add(1);
            auto [e3] =
                co_await c3.connect(endpoint(ipv4_address::loopback(), port));
            if (!e3)
                connected->fetch_add(1);

            // Give the server time to register the connections.
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(50));

            // Disconnect middle first, then tail, then head:
            // exercises remove from each list position.
            c2.close();
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));
            c3.close();
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));
            c1.close();
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));

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
        io_context ioc(Backend);

        class throwing_worker : public tcp_server::worker_base
        {
            io_context& ctx_;
            corosio::tcp_socket sock_;

        public:
            std::atomic<bool>* threw = nullptr;

            throwing_worker(io_context& ctx, std::atomic<bool>* t)
                : ctx_(ctx)
                , sock_(ctx)
                , threw(t)
            {
            }

            corosio::tcp_socket& socket() override
            {
                return sock_;
            }

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
            BOOST_TEST(!client.open());
            [[maybe_unused]] auto [cec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            client.close();

            // Give server time to handle the connection, then stop.
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(50));
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
        io_context ioc(Backend);
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
        // return (exercises pop_awaitable suspend / worker return wake).
        io_context ioc(Backend);

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
                BOOST_TEST(!client.open());
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

    void testLauncherDropWakesWaitingAccept()
    {
        // A worker parks its launcher, so the pool is empty when the
        // next connection arrives and the accept loop waits for a
        // worker. Destroying the parked launcher must hand the worker
        // straight to that waiter.
        io_context ioc(Backend);

        class parking_worker : public tcp_server::worker_base
        {
            corosio::tcp_socket sock_;

        public:
            std::optional<tcp_server::launcher>* slot = nullptr;
            std::atomic<int>* run_count               = nullptr;

            parking_worker(
                io_context& ctx,
                std::optional<tcp_server::launcher>* s,
                std::atomic<int>* c)
                : sock_(ctx)
                , slot(s)
                , run_count(c)
            {
            }

            corosio::tcp_socket& socket() override
            {
                return sock_;
            }

            void run(tcp_server::launcher launch) override
            {
                sock_.close();
                if (run_count->fetch_add(1) == 0)
                    slot->emplace(std::move(launch));
            }
        };

        std::optional<tcp_server::launcher> parked;
        std::atomic<int> run_count{0};

        class parking_server : public tcp_server
        {
        public:
            parking_server(
                io_context& ctx,
                std::optional<tcp_server::launcher>* s,
                std::atomic<int>* c)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<parking_worker>(ctx, s, c));
                set_workers(std::move(v));
            }
        };

        parking_server srv(ioc, &parked, &run_count);
        auto ec = srv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = srv.local_endpoint().port();

        srv.start();

        auto driver =
            [](io_context* ioc, std::uint16_t port, parking_server* srv,
               std::optional<tcp_server::launcher>* parked) -> capy::task<> {
            tcp_socket c1(*ioc);
            BOOST_TEST(!c1.open());
            [[maybe_unused]] auto [e1] =
                co_await c1.connect(endpoint(ipv4_address::loopback(), port));
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));

            // Pool is now empty; this connection parks the accept loop.
            tcp_socket c2(*ioc);
            BOOST_TEST(!c2.open());
            [[maybe_unused]] auto [e2] =
                co_await c2.connect(endpoint(ipv4_address::loopback(), port));
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));

            // Dropping the parked launcher wakes the waiting accept.
            parked->reset();
            std::ignore =
                co_await corosio::delay(std::chrono::milliseconds(20));

            c1.close();
            c2.close();
            srv->stop();
        }(&ioc, port, &srv, &parked);

        capy::run_async(ioc.get_executor())(std::move(driver));
        ioc.run();
        srv.join();

        BOOST_TEST_EQ(run_count.load(), 2);
    }

    // Sends one byte through a fresh connection and waits for the echo.
    static bool echo_once(io_context& ioc, endpoint ep)
    {
        tcp_socket c(ioc);
        bool ok = false;
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep, bool& ok) -> capy::task<> {
                auto [cec] = co_await c.connect(ep);
                if (cec)
                    co_return;
                char out = 'x';
                auto [wec, wn] =
                    co_await c.write_some(capy::const_buffer(&out, 1));
                if (wec)
                    co_return;
                char in   = 0;
                auto [rec, rn] =
                    co_await c.read_some(capy::mutable_buffer(&in, 1));
                ok = !rec && rn == 1 && in == 'x';
            }(c, ep, ok));
        ioc.restart();
        // Bounds a hang, not a timing assertion.
        while (!ok && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        return ok;
    }

    // Destroying a running server stops it: its accept loops and the
    // workers they launched finish without touching the destroyed
    // object, and its listeners close.
    void testDestroyWhileRunning()
    {
        io_context ioc(Backend);
        endpoint ep;
        {
            test_server srv(ioc);
            BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
            ep = srv.local_endpoint();
            srv.start();
            BOOST_TEST(echo_once(ioc, ep));
        }
        ioc.restart();
        ioc.run(); // returns: nothing of the server is left running

        tcp_socket c(ioc);
        std::error_code cec;
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep, std::error_code& ec)
                -> capy::task<> {
                auto [e] = co_await c.connect(ep);
                ec       = e;
            }(c, ep, cec));
        ioc.restart();
        ioc.run();
        BOOST_TEST(!!cec); // the listener is closed
    }

    // A running server keeps serving after it is moved.
    void testMoveWhileRunning()
    {
        io_context ioc(Backend);
        test_server src(ioc);
        BOOST_TEST(!src.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = src.local_endpoint();
        src.start();
        BOOST_TEST(echo_once(ioc, ep));

        {
            test_server dst(std::move(src));
            BOOST_TEST(echo_once(ioc, ep));
            BOOST_TEST(echo_once(ioc, ep));
        }
        ioc.restart();
        ioc.run(); // the destroyed server's loops finish
    }

    // Workers return from several run threads at once.
    void testConcurrentWorkersOnMultithreadedContext()
    {
        io_context ioc(Backend);
        auto srv = std::make_unique<test_server>(ioc);
        BOOST_TEST(!srv->bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv->local_endpoint();
        srv->start();

        std::vector<std::thread> runners;
        for (int i = 0; i < 4; ++i)
            runners.emplace_back([&] { ioc.run(); });

        std::atomic<int> echoed{0};
        std::vector<std::thread> clients;
        for (int i = 0; i < 4; ++i)
            clients.emplace_back([&] {
                io_context cctx(Backend);
                for (int k = 0; k < 25; ++k)
                    if (echo_once(cctx, ep))
                        ++echoed;
            });
        for (auto& t : clients)
            t.join();
        BOOST_TEST_EQ(echoed.load(), 100);

        ioc.stop();
        for (auto& t : runners)
            t.join();
        srv.reset();
        ioc.restart();
        ioc.run(); // the destroyed server's loops finish
    }

    // Connects and closes at once, so connections keep arriving. The
    // close resets: a graceful one would park each connection's port
    // in TIME_WAIT, and at this rate that drains the ephemeral range
    // for every test running alongside.
    static void connect_once(io_context& ioc, endpoint ep)
    {
        tcp_socket c(ioc);
        BOOST_TEST(!c.open());
        test::must_ok(c.set_option(socket_option::linger(true, 0)));
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep) -> capy::task<> {
                std::ignore = co_await c.connect(ep);
            }(c, ep));
        ioc.restart();
        ioc.run();
    }

    static std::error_code connect_result(io_context& ioc, endpoint ep)
    {
        tcp_socket c(ioc);
        std::error_code ec;
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep, std::error_code& ec)
                -> capy::task<> {
                auto [e] = co_await c.connect(ep);
                ec       = e;
            }(c, ep, ec));
        ioc.restart();
        ioc.run();
        return ec;
    }

    // A worker that keeps its launcher instead of starting it, so the
    // test decides when, or whether, the connection runs.
    class holding_worker : public tcp_server::worker_base
    {
        corosio::tcp_socket sock_;
        std::optional<tcp_server::launcher>* slot_;

    public:
        holding_worker(
            io_context& ctx, std::optional<tcp_server::launcher>* s)
            : sock_(ctx)
            , slot_(s)
        {
        }

        corosio::tcp_socket& socket() override
        {
            return sock_;
        }

        void run(tcp_server::launcher launch) override
        {
            slot_->emplace(std::move(launch));
        }
    };

    class holding_server : public tcp_server
    {
    public:
        holding_server(
            io_context& ctx, std::optional<tcp_server::launcher>* s)
            : tcp_server(ctx, ctx.get_executor())
        {
            std::vector<std::unique_ptr<tcp_server::worker_base>> v;
            v.push_back(std::make_unique<holding_worker>(ctx, s));
            set_workers(std::move(v));
        }
    };

    // An accept loop parked for a worker, because the only worker is
    // still busy, must not keep the listener open once the server is
    // destroyed.
    void testDestroyWithBusyWorkerClosesListener()
    {
        io_context ioc(Backend);
        std::optional<tcp_server::launcher> held;

        auto srv = std::make_unique<holding_server>(ioc, &held);
        BOOST_TEST(!srv->bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv->local_endpoint();
        srv->start();

        io_context cctx;
        BOOST_TEST(!connect_result(cctx, ep));
        // Bounds a hang, not a timing assertion.
        while (!held && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(held.has_value());
        while (ioc.poll() != 0)
        {
        }

        srv.reset();
        while (ioc.poll() != 0)
        {
        }

        BOOST_TEST(!!connect_result(cctx, ep));

        held.reset();
        ioc.restart();
        ioc.run();
    }

    // A connection dispatched before stop() but launched after it
    // must still see the stop.
    void testStopReachesLaunchAfterStop()
    {
        io_context ioc(Backend);
        std::optional<tcp_server::launcher> held;
        holding_server srv(ioc, &held);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv.local_endpoint();
        srv.start();

        io_context cctx;
        BOOST_TEST(!connect_result(cctx, ep));
        // Bounds a hang, not a timing assertion.
        while (!held && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(held.has_value());

        srv.stop();
        while (ioc.poll() != 0)
        {
        }

        bool done     = false;
        bool canceled = false;
        (*held)(
            ioc.get_executor(),
            [](bool& d, bool& c) -> capy::task<> {
                auto [ec] = co_await corosio::delay(std::chrono::hours(1));
                c         = ec == capy::cond::canceled;
                d         = true;
            }(done, canceled));
        held.reset();
        ioc.restart();
        while (!done && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(done);
        BOOST_TEST(canceled);
        ioc.run();
        srv.join();
    }

    // The connection a launcher starts outlives the chain that invoked
    // it, so it must not be allocated from that chain's allocator.
    void testLateLaunchOutlivesInvokingChain()
    {
        io_context ioc(Backend);
        std::optional<tcp_server::launcher> held;
        holding_server srv(ioc, &held);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv.local_endpoint();
        srv.start();

        io_context cctx;
        BOOST_TEST(!connect_result(cctx, ep));
        // Bounds a hang, not a timing assertion.
        while (!held && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(held.has_value());

        counting_resource mr;
        bool done    = false;
        auto session = []() -> capy::task<> { co_return; }();
        capy::run_async(ioc.get_executor(), &mr, [&]() { done = true; })(
            [](tcp_server::launcher& l, capy::task<> t,
               io_context::executor_type ex) -> capy::task<> {
                l(ex, std::move(t));
                co_return;
            }(*held, std::move(session), ioc.get_executor()));
        while (!done && ioc.run_one() != 0)
        {
        }
        BOOST_TEST(done);
        BOOST_TEST_EQ(mr.live, 0);

        held.reset();
        srv.stop();
        ioc.run();
        srv.join();
    }

    struct overlap_counts
    {
        std::atomic<int> in_run{0};
        std::atomic<int> max_in_run{0};
        std::atomic<int> runs{0};
    };

    // Starts its connection on another context, then lingers in run()
    // until a second run() begins or a bound passes.
    class lingering_worker : public tcp_server::worker_base
    {
        corosio::tcp_socket sock_;
        io_context& session_ctx_;
        overlap_counts& counts_;

    public:
        lingering_worker(
            io_context& ctx, io_context& session_ctx, overlap_counts& counts)
            : sock_(ctx)
            , session_ctx_(session_ctx)
            , counts_(counts)
        {
        }

        corosio::tcp_socket& socket() override
        {
            return sock_;
        }

        void run(tcp_server::launcher launch) override
        {
            int const n = ++counts_.in_run;
            int seen    = counts_.max_in_run.load();
            while (n > seen &&
                   !counts_.max_in_run.compare_exchange_weak(seen, n))
            {
            }
            ++counts_.runs;
            launch(
                session_ctx_.get_executor(),
                [](corosio::tcp_socket* s) -> capy::task<> {
                    s->close();
                    co_return;
                }(&sock_));
            // Bounds the wait, not a timing assertion.
            auto const until = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(500);
            while (counts_.runs.load() < 2 &&
                   std::chrono::steady_clock::now() < until)
                std::this_thread::yield();
            --counts_.in_run;
        }
    };

    // Reads one byte from its socket on another context.
    class reading_worker : public tcp_server::worker_base
    {
        corosio::tcp_socket sock_;
        io_context& session_ctx_;
        bool& launched_;

    public:
        reading_worker(io_context& ctx, io_context& session_ctx, bool& l)
            : sock_(ctx)
            , session_ctx_(session_ctx)
            , launched_(l)
        {
        }

        corosio::tcp_socket& socket() override
        {
            return sock_;
        }

        void run(tcp_server::launcher launch) override
        {
            launched_ = true;
            launch(
                session_ctx_.get_executor(),
                [](corosio::tcp_socket& s) -> capy::task<> {
                    char c;
                    std::ignore =
                        co_await s.read_some(capy::mutable_buffer(&c, 1));
                }(sock_));
        }
    };

    // A connection is work on the context it runs on, so that context
    // does not run out of work while the connection waits elsewhere.
    void testSessionHoldsWorkOnItsContext()
    {
        io_context session_ctx(Backend);
        io_context ioc(Backend);
        bool launched = false;

        class server : public tcp_server
        {
        public:
            server(io_context& ctx, io_context& sctx, bool& l)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<reading_worker>(ctx, sctx, l));
                set_workers(std::move(v));
            }
        };

        server srv(ioc, session_ctx, launched);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        srv.start();
        tcp_socket client(ioc);
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep) -> capy::task<> {
                std::ignore = co_await c.connect(ep);
            }(client, srv.local_endpoint()));
        // Bounds a hang, not a timing assertion.
        while (!launched && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(launched);

        std::ignore = session_ctx.poll();
        BOOST_TEST(!session_ctx.stopped());

        // The connection ends, and with it the work.
        client.close();
        while (!session_ctx.stopped() &&
               ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
            std::ignore = session_ctx.poll();
        }
        BOOST_TEST(session_ctx.stopped());

        srv.stop();
        std::ignore = ioc.run();
        srv.join();
    }

    // A finished connection's hop back to the server's executor, left
    // queued when that context is destroyed, is torn down there, never
    // sent to the session's context, which may already be gone.
    void testQueuedHopTornDownOnServerContext()
    {
        auto session_ctx = std::make_unique<io_context>(Backend);
        auto ioc         = std::make_unique<io_context>(Backend);
        bool launched    = false;

        class server : public tcp_server
        {
        public:
            server(io_context& ctx, io_context& sctx, bool& l)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<reading_worker>(ctx, sctx, l));
                set_workers(std::move(v));
            }
        };

        {
            server srv(*ioc, *session_ctx, launched);
            BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
            srv.start();
            tcp_socket client(*ioc);
            capy::run_async(ioc->get_executor())(
                [](tcp_socket& c, endpoint ep) -> capy::task<> {
                    std::ignore = co_await c.connect(ep);
                }(client, srv.local_endpoint()));
            // Bounds a hang, not a timing assertion.
            while (!launched && ioc->run_one_for(std::chrono::seconds(5)) != 0)
            {
            }
            BOOST_TEST(launched);
            std::ignore = session_ctx->poll();

            client.close();
            while (!session_ctx->stopped() &&
                   ioc->run_one_for(std::chrono::seconds(5)) != 0)
            {
                std::ignore = session_ctx->poll();
            }
            BOOST_TEST(session_ctx->stopped());
            srv.stop();
        }
        session_ctx.reset();
        ioc.reset();
    }

    // Holds what is posted or dispatched to it until the test runs it
    // or gives it up, as a context's queue does.
    struct held_executor
    {
        io_context* ctx_;
        std::deque<capy::continuation>* q_;

        bool operator==(held_executor const& o) const noexcept
        {
            return q_ == o.q_;
        }

        io_context& context() const noexcept
        {
            return *ctx_;
        }

        void on_work_started() const noexcept {}
        void on_work_finished() const noexcept {}

        std::coroutine_handle<> dispatch(capy::continuation& c) const
        {
            q_->push_back(c);
            return std::noop_coroutine();
        }

        void post(capy::continuation& c) const
        {
            q_->push_back(c);
        }
    };

    // Counts work; refuses every post.
    struct refusing_executor
    {
        io_context* ctx_;
        int* work_;

        bool operator==(refusing_executor const& o) const noexcept
        {
            return work_ == o.work_;
        }

        io_context& context() const noexcept
        {
            return *ctx_;
        }

        void on_work_started() const noexcept
        {
            ++*work_;
        }

        void on_work_finished() const noexcept
        {
            --*work_;
        }

        std::coroutine_handle<> dispatch(capy::continuation&) const
        {
            throw std::runtime_error("dispatch");
        }

        void post(capy::continuation&) const
        {
            throw std::runtime_error("post");
        }
    };

    struct destroy_count
    {
        int* n;

        explicit destroy_count(int& c) noexcept
            : n(&c)
        {
        }

        destroy_count(destroy_count&& o) noexcept
            : n(std::exchange(o.n, nullptr))
        {
        }

        ~destroy_count()
        {
            if (n)
                ++*n;
        }
    };

    // A launch whose post throws hands the exception to the worker and
    // gives back what it took: the task, its work, and the worker.
    void testLaunchPostThrows()
    {
        io_context ioc(Backend);
        int work = 0, runs = 0, caught = 0, down = 0;

        class refused_worker : public tcp_server::worker_base
        {
            corosio::tcp_socket sock_;
            io_context& ctx_;
            int& work_;
            int& runs_;
            int& caught_;
            int& down_;

        public:
            refused_worker(
                io_context& ctx, int& work, int& runs, int& caught, int& down)
                : sock_(ctx)
                , ctx_(ctx)
                , work_(work)
                , runs_(runs)
                , caught_(caught)
                , down_(down)
            {
            }

            corosio::tcp_socket& socket() override
            {
                return sock_;
            }

            void run(tcp_server::launcher launch) override
            {
                ++runs_;
                sock_.close();
                try
                {
                    launch(
                        refusing_executor{&ctx_, &work_},
                        [](destroy_count) -> capy::task<> { co_return; }(
                            destroy_count(down_)));
                }
                catch (std::runtime_error const&)
                {
                    ++caught_;
                }
            }
        };

        class server : public tcp_server
        {
        public:
            server(io_context& ctx, int& w, int& r, int& c, int& d)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<refused_worker>(ctx, w, r, c, d));
                set_workers(std::move(v));
            }
        };

        server srv(ioc, work, runs, caught, down);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        srv.start();
        tcp_socket c1(ioc), c2(ioc);
        auto connect = [](tcp_socket& c, endpoint ep) -> capy::task<> {
            std::ignore = co_await c.connect(ep);
        };
        capy::run_async(ioc.get_executor())(connect(c1, srv.local_endpoint()));
        capy::run_async(ioc.get_executor())(connect(c2, srv.local_endpoint()));
        // Bounds a hang, not a timing assertion.
        while (runs < 2 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST_EQ(runs, 2);
        BOOST_TEST_EQ(caught, 2);
        BOOST_TEST_EQ(down, 2);
        BOOST_TEST_EQ(work, 0);
        srv.stop();
        std::ignore = ioc.run();
        srv.join();
    }

    // An accept loop's hop back to the server's executor, given up while
    // queued there, unwinds the loop from the coroutine awaiting it
    // rather than destroying the loop's frame under that coroutine.
    void testQueuedAcceptLoopHopGivenUp()
    {
        io_context ioc(Backend);
        std::deque<capy::continuation> q;
        bool launched = false;

        class server : public tcp_server
        {
        public:
            server(io_context& ctx, held_executor ex, bool& l)
                : tcp_server(ctx, ex)
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<reading_worker>(ctx, ctx, l));
                set_workers(std::move(v));
            }
        };

        auto run_queued = [&q](std::size_t n)
        {
            while (n-- != 0 && !q.empty())
            {
                auto c = q.front();
                q.pop_front();
                c.h.resume();
            }
        };

        {
            server srv(ioc, held_executor{&ioc, &q}, launched);
            BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
            srv.start();
            run_queued(q.size()); // the loop parks its accept
            srv.stop();
            std::ignore = ioc.poll(); // the canceled accept is queued
            run_queued(q.size()); // the loop hops back, and stays queued
            while (!q.empty())
            {
                auto c = q.front();
                q.pop_front();
                capy::abandon(c);
            }
        }
        BOOST_TEST(!launched);
    }

    // A worker is not handed to another connection while its run() is
    // still executing, even once the connection it launched is done.
    void testWorkerNotReusedDuringRun()
    {
        io_context session_ctx(Backend);
        io_context ioc(Backend);
        overlap_counts counts;

        class server : public tcp_server
        {
        public:
            server(io_context& ctx, io_context& sctx, overlap_counts& c)
                : tcp_server(ctx, ctx.get_executor())
            {
                std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                v.push_back(std::make_unique<lingering_worker>(ctx, sctx, c));
                set_workers(std::move(v));
            }
        };

        server srv(ioc, session_ctx, counts);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        io_context cctx;
        BOOST_TEST(!connect_result(cctx, srv.local_endpoint(0)));
        BOOST_TEST(!connect_result(cctx, srv.local_endpoint(1)));

        auto sex = session_ctx.get_executor();
        sex.on_work_started();
        std::thread session_thread([&] { session_ctx.run(); });

        srv.start();
        std::thread t1([&] { ioc.run(); });
        std::thread t2([&] { ioc.run(); });

        // Bounds a hang, not a timing assertion.
        auto const until =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while ((counts.runs.load() < 2 || counts.in_run.load() != 0) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::yield();

        srv.stop();
        t1.join();
        t2.join();
        srv.join();
        sex.on_work_finished();
        session_thread.join();

        BOOST_TEST_EQ(counts.runs.load(), 2);
        BOOST_TEST_EQ(counts.max_in_run.load(), 1);
    }

    struct destroy_count_t
    {
        int* n;
        explicit destroy_count_t(int& c) : n(&c) {}
        destroy_count_t(destroy_count_t&& o) noexcept
            : n(std::exchange(o.n, nullptr))
        {
        }
        ~destroy_count_t()
        {
            if (n)
                ++*n;
        }
    };

    // Connects a socket the caller keeps open.
    static void connect_kept(io_context& ioc, tcp_socket& c, endpoint ep)
    {
        std::error_code ec;
        capy::run_async(ioc.get_executor())(
            [](tcp_socket& c, endpoint ep,
               std::error_code& ec) -> capy::task<> {
                auto [e] = co_await c.connect(ep);
                ec       = e;
            }(c, ep, ec));
        ioc.restart();
        ioc.run();
        BOOST_TEST(!ec);
    }

    // Its first connection reads forever on a socket pair of another
    // context, which owns nothing else of the server; later
    // connections are declined.
    class session_worker : public tcp_server::worker_base
    {
        corosio::tcp_socket sock_;
        io_context& session_ctx_;
        int& runs_;
        int& down_;

    public:
        session_worker(io_context& ctx, io_context& sctx, int& runs, int& down)
            : sock_(ctx)
            , session_ctx_(sctx)
            , runs_(runs)
            , down_(down)
        {
        }

        corosio::tcp_socket& socket() override
        {
            return sock_;
        }

        void run(tcp_server::launcher launch) override
        {
            sock_.close();
            if (runs_++ != 0)
                return;
            auto [a, b] = test::make_socket_pair(session_ctx_);
            launch(
                session_ctx_.get_executor(),
                [](tcp_socket s, tcp_socket, destroy_count_t) -> capy::task<> {
                    char c;
                    std::ignore =
                        co_await s.read_some(capy::mutable_buffer(&c, 1));
                }(std::move(a), std::move(b), destroy_count_t(down_)));
        }
    };

    class session_server : public tcp_server
    {
    public:
        session_server(io_context& ctx, io_context& sctx, int& runs, int& down)
            : tcp_server(ctx, ctx.get_executor())
        {
            std::vector<std::unique_ptr<tcp_server::worker_base>> v;
            v.push_back(
                std::make_unique<session_worker>(ctx, sctx, runs, down));
            set_workers(std::move(v));
        }
    };

    // Tearing down the context a connection runs on returns its worker,
    // so an accept loop waiting for one gets it.
    void testSessionTeardownWakesWaitingAccept()
    {
        int runs = 0, down = 0;
        io_context ioc(Backend);
        std::optional<io_context> session_ctx(std::in_place, Backend);
        session_server srv(ioc, *session_ctx, runs, down);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv.local_endpoint();
        srv.start();

        io_context cctx;
        tcp_socket c1(cctx), c2(cctx);
        connect_kept(cctx, c1, ep);
        // Bounds a hang, not a timing assertion.
        while (runs < 1 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST_EQ(runs, 1);
        std::ignore = session_ctx->poll();
        connect_kept(cctx, c2, ep);
        while (ioc.poll() != 0)
        {
        }
        BOOST_TEST_EQ(runs, 1); // the accept loop waits for the worker

        session_ctx.reset();
        BOOST_TEST_EQ(down, 1);

        ioc.restart();
        while (runs < 2 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST_EQ(runs, 2);

        srv.stop();
        ioc.restart();
        ioc.run();
        srv.join();
    }

    // A connection whose context is torn down, before it starts or
    // while it reads, returns its worker, and the server outlives
    // that context.
    void testSessionTeardownReturnsWorker()
    {
        for (bool started : {false, true})
        {
            int runs = 0, down = 0;
            io_context ioc(Backend);
            std::optional<io_context> session_ctx(std::in_place, Backend);
            {
                session_server srv(ioc, *session_ctx, runs, down);
                BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
                auto ep = srv.local_endpoint();
                srv.start();

                io_context cctx;
                tcp_socket c(cctx);
                connect_kept(cctx, c, ep);
                // Bounds a hang, not a timing assertion.
                while (runs < 1 &&
                       ioc.run_one_for(std::chrono::seconds(5)) != 0)
                {
                }
                BOOST_TEST_EQ(runs, 1);
                if (started)
                    std::ignore = session_ctx->poll();
                BOOST_TEST_EQ(down, 0);

                session_ctx.reset();
                BOOST_TEST_EQ(down, 1);

                srv.stop();
                ioc.restart();
                ioc.run();
                srv.join();
                // No worker is still lent.
                BOOST_TEST_NO_THROW(srv.set_workers(make_test_workers(ioc, 1)));
            }
            BOOST_TEST_EQ(down, 1);
        }
    }

    // Destroying a connection that never started destroys its task
    // before returning the worker, so nothing the task's parameters
    // release can race the worker's next connection.
    void testUnstartedLaunchKeepsWorkerUntilTaskDestroyed()
    {
        io_context ioc(Backend);
        std::optional<io_context> session_ctx(std::in_place, Backend);
        std::optional<tcp_server::launcher> held;
        holding_server srv(ioc, &held);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv.local_endpoint();
        srv.start();

        io_context cctx;
        BOOST_TEST(!connect_result(cctx, ep));
        // Bounds a hang, not a timing assertion.
        while (!held && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(held.has_value());

        struct lent_probe
        {
            holding_server* srv;
            io_context* ioc;
            bool* checked;
            lent_probe(holding_server& s, io_context& c, bool& b)
                : srv(&s)
                , ioc(&c)
                , checked(&b)
            {
            }
            lent_probe(lent_probe&& o) noexcept
                : srv(std::exchange(o.srv, nullptr))
                , ioc(o.ioc)
                , checked(o.checked)
            {
            }
            ~lent_probe()
            {
                if (!srv)
                    return;
                *checked = true;
                BOOST_TEST_THROWS(
                    srv->set_workers(make_test_workers(*ioc, 1)),
                    std::logic_error);
            }
        };

        bool checked = false;
        (*held)(session_ctx->get_executor(), [](lent_probe) -> capy::task<> {
            co_return;
        }(lent_probe(srv, ioc, checked)));
        held.reset();
        // With no accept loop left to take it, a returned worker goes
        // back to the pool, where set_workers would release it.
        srv.stop();
        ioc.restart();
        ioc.run();
        srv.join();

        session_ctx.reset();
        BOOST_TEST(checked);
    }

    // A server owned by a coroutine the context's teardown destroys,
    // with its accept loop waiting for the only worker, which is busy
    // in a read: every frame goes with the context.
    void testTeardownWithAcceptWaitingForWorker()
    {
        int run_down = 0, owner_down = 0;
        io_context cctx;
        tcp_socket c1(cctx), c2(cctx);
        {
            io_context ioc(Backend);
            endpoint ep;
            int runs = 0;

            class reading_worker : public tcp_server::worker_base
            {
                corosio::tcp_socket sock_;
                io_context& ctx_;
                int& runs_;
                int& down_;

            public:
                reading_worker(io_context& ctx, int& runs, int& down)
                    : sock_(ctx)
                    , ctx_(ctx)
                    , runs_(runs)
                    , down_(down)
                {
                }

                corosio::tcp_socket& socket() override
                {
                    return sock_;
                }

                void run(tcp_server::launcher launch) override
                {
                    ++runs_;
                    launch(
                        ctx_.get_executor(),
                        [](tcp_socket& s, destroy_count_t) -> capy::task<> {
                            char c;
                            std::ignore = co_await s.read_some(
                                capy::mutable_buffer(&c, 1));
                        }(sock_, destroy_count_t(down_)));
                }
            };

            class reading_server : public tcp_server
            {
            public:
                reading_server(io_context& ctx, int& runs, int& down)
                    : tcp_server(ctx, ctx.get_executor())
                {
                    std::vector<std::unique_ptr<tcp_server::worker_base>> v;
                    v.push_back(
                        std::make_unique<reading_worker>(ctx, runs, down));
                    set_workers(std::move(v));
                }
            };

            capy::run_async(ioc.get_executor())(
                [](io_context& ioc, endpoint& ep, int& runs, int& run_down,
                   destroy_count_t) -> capy::task<> {
                    reading_server srv(ioc, runs, run_down);
                    BOOST_TEST(
                        !srv.bind(endpoint(ipv4_address::loopback(), 0)));
                    ep = srv.local_endpoint();
                    srv.start();
                    std::ignore =
                        co_await corosio::delay(std::chrono::hours(1));
                }(ioc, ep, runs, run_down, destroy_count_t(owner_down)));
            std::ignore = ioc.poll();
            BOOST_TEST_NE(ep.port(), 0);

            connect_kept(cctx, c1, ep);
            // Bounds a hang, not a timing assertion.
            while (runs < 1 && ioc.run_one_for(std::chrono::seconds(5)) != 0)
            {
            }
            BOOST_TEST_EQ(runs, 1);
            connect_kept(cctx, c2, ep);
            while (ioc.poll() != 0)
            {
            }
            BOOST_TEST_EQ(runs, 1);
            BOOST_TEST_EQ(run_down, 0);
            BOOST_TEST_EQ(owner_down, 0);
        }
        BOOST_TEST_EQ(run_down, 1);
        BOOST_TEST_EQ(owner_down, 1);
    }

    // An accept loop holds a worker while it waits for a connection,
    // so replacing the workers then would free one in use.
    void testSetWorkersWhileAcceptingThrows()
    {
        io_context ioc(Backend);
        test_server srv(ioc);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        srv.start();
        while (ioc.poll() != 0)
        {
        }

        BOOST_TEST_THROWS(
            srv.set_workers(make_test_workers(ioc, 1)), std::logic_error);

        srv.stop();
        ioc.run();
        srv.join();
        srv.set_workers(make_test_workers(ioc, 1));
    }

    // A launcher kept past join still borrows its worker.
    void testSetWorkersWithBorrowedWorkerThrows()
    {
        io_context ioc(Backend);
        std::optional<tcp_server::launcher> held;
        holding_server srv(ioc, &held);
        BOOST_TEST(!srv.bind(endpoint(ipv4_address::loopback(), 0)));
        auto ep = srv.local_endpoint();
        srv.start();

        io_context cctx;
        BOOST_TEST(!connect_result(cctx, ep));
        // Bounds a hang, not a timing assertion.
        while (!held && ioc.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(held.has_value());

        srv.stop();
        ioc.run();
        srv.join();
        BOOST_TEST_THROWS(
            srv.set_workers(make_test_workers(ioc, 1)), std::logic_error);

        held.reset();
        srv.set_workers(make_test_workers(ioc, 1));
    }

    // Destroying the server from a thread that is not running its
    // executor must not race the accept loops still running there.
    void testDestroyWhileRunnersAccept()
    {
        for (int round = 0; round < 20; ++round)
        {
            io_context ioc(Backend);
            auto srv = std::make_unique<test_server>(ioc);
            BOOST_TEST(!srv->bind(endpoint(ipv4_address::loopback(), 0)));
            auto ep = srv->local_endpoint();
            srv->start();

            std::vector<std::thread> runners;
            for (int i = 0; i < 4; ++i)
                runners.emplace_back([&] { ioc.run(); });

            std::atomic<bool> quit{false};
            std::atomic<int> connects{0};
            std::vector<std::thread> clients;
            // The default backend for the clients: io_uring maps its
            // rings with raw syscalls the thread sanitizer cannot see,
            // so each round's client threads would appear to race the
            // last round's over reused addresses.
            for (int i = 0; i < 3; ++i)
                clients.emplace_back([&] {
                    io_context cctx;
                    while (!quit.load())
                    {
                        connect_once(cctx, ep);
                        ++connects;
                    }
                });
            while (connects.load() < 20)
                std::this_thread::yield();

            srv.reset();
            quit = true;
            for (auto& t : clients)
                t.join();
            ioc.stop();
            for (auto& t : runners)
                t.join();
            ioc.restart();
            ioc.run(); // the destroyed server's loops finish
        }
        BOOST_TEST_PASS();
    }

    void run()
    {
        testLauncherDropWakesWaitingAccept();
        testStopServer();
        testStopWithActiveConnection();
        testStartIdempotent();
        testStopIdempotent();
        testStopWithoutStart();
        testRestart();
        testStartWithoutJoinThrows();
        testListenErrorCode();
        testBindSuccess();
        testBindCollision();
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
        testDestroyWhileRunning();
        testMoveWhileRunning();
        testConcurrentWorkersOnMultithreadedContext();
        testDestroyWhileRunnersAccept();
        testDestroyWithBusyWorkerClosesListener();
        testStopReachesLaunchAfterStop();
        testLateLaunchOutlivesInvokingChain();
        testQueuedHopTornDownOnServerContext();
        testQueuedAcceptLoopHopGivenUp();
        testLaunchPostThrows();
        testWorkerNotReusedDuringRun();
        testSessionHoldsWorkOnItsContext();
        testSessionTeardownWakesWaitingAccept();
        testSessionTeardownReturnsWorker();
        testTeardownWithAcceptWaitingForWorker();
        testUnstartedLaunchKeepsWorkerUntilTaskDestroyed();
        testSetWorkersWhileAcceptingThrows();
        testSetWorkersWithBorrowedWorkerThrows();
    }
};

COROSIO_BACKEND_TESTS(tcp_server_test, "boost.corosio.tcp_server")

} // namespace boost::corosio
