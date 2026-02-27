//
// Copyright (c) 2026 Vinnie Falco
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Stress tests to expose race conditions in the async I/O scheduler.
// These tests hammer on specific code paths that have potential
// race conditions, running many iterations to increase the
// likelihood of triggering intermittent bugs.
//
// Target areas:
// 1. stop_callback lifetime - stop token firing during completion
// 2. ready_ flag race - synchronous completion vs completion port delivery
// 3. Rapid cancel/complete cycles
// 4. shared_ptr lifetime - operation completion vs tcp_socket close race
//
// Tests run on all backends (epoll, IOCP, select).

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/timer.hpp>

#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {
namespace {

// Default stress test duration in seconds (can be overridden via command line)
constexpr int default_stress_seconds = 1;

int
get_stress_duration()
{
    auto* opt = test_suite::get_command_line_option("stress-duration");
    if (opt)
        return std::atoi(opt);
    return default_stress_seconds;
}

// Create a connected tcp_socket pair for stress testing.
// Uses ephemeral port (0) so the OS assigns an available port,
// avoiding TIME_WAIT collisions on back-to-back runs.
std::pair<tcp_socket, tcp_socket>
make_stress_pair(io_context& ctx)
{
    auto ex = ctx.get_executor();

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done  = false;
    bool connect_done = false;

    tcp_acceptor acc(ctx);
    acc.open();
    acc.set_option(socket_option::reuse_address(true));
    if (auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0)))
        throw std::runtime_error("stress_pair bind failed: " + ec.message());
    if (auto ec = acc.listen())
        throw std::runtime_error("stress_pair listen failed: " + ec.message());
    auto port = acc.local_endpoint().port();

    tcp_socket s1(ctx);
    tcp_socket s2(ctx);
    s2.open();

    capy::run_async(ex)(
        [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await a.accept(s);
            ec_out    = ec;
            done_out  = true;
        }(acc, s1, accept_ec, accept_done));

    capy::run_async(ex)(
        [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
           bool& done_out) -> capy::task<> {
            auto [ec] = co_await s.connect(ep);
            ec_out    = ec;
            done_out  = true;
        }(s2, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

    ctx.run();
    ctx.restart();

    if (!accept_done || accept_ec)
        throw std::runtime_error("stress_pair accept failed");
    if (!connect_done || connect_ec)
        throw std::runtime_error("stress_pair connect failed");

    acc.close();
    return {std::move(s1), std::move(s2)};
}

} // namespace

// Stress Test 1: Stop Token Cancellation Race
//
// This test hammers on the race between stop_token firing and
// operation completion. The stop_callback could fire at any point:
// - Before the I/O starts
// - While I/O is pending
// - After IOCP delivers completion but before operator() runs
// - During operator() execution

template<auto Backend>
struct stop_token_stress_test
{
    void run()
    {
        int duration = get_stress_duration();
        std::fprintf(
            stderr, "  stop_token_stress: running for %d seconds...\n",
            duration);

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Pre-create tcp_socket pair BEFORE ioc.run()
        auto [s1, s2] = make_stress_pair(ioc);

        std::atomic<std::size_t> iterations{0};
        std::atomic<std::size_t> cancellations{0};
        std::atomic<std::size_t> completions{0};
        std::atomic<bool> stop_flag{false};

        // Worker task: rapidly start reads and cancel via stop_token
        auto worker = [&]() -> capy::task<> {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                try
                {
                    // Rapidly cycle through cancel scenarios
                    for (int i = 0;
                         i < 100 && !stop_flag.load(std::memory_order_relaxed);
                         ++i)
                    {
                        std::stop_source stop_src;

                        // Start read with stop token
                        char buf[32];
                        std::atomic<bool> read_done{false};
                        std::error_code read_ec;

                        auto read_coro = [&read_done, &read_ec, &s2,
                                          &buf]() -> capy::task<> {
                            auto [ec, n] = co_await s2.read_some(
                                capy::mutable_buffer(buf, sizeof(buf)));
                            read_ec = ec;
                            read_done.store(true, std::memory_order_release);
                        };

                        capy::run_async(ex, stop_src.get_token())(read_coro());

                        // Vary timing: sometimes cancel immediately, sometimes after brief delay
                        if (i % 3 == 0)
                        {
                            // Immediate cancel
                            stop_src.request_stop();
                        }
                        else if (i % 3 == 1)
                        {
                            // Brief delay then cancel
                            timer delay(ioc);
                            delay.expires_after(std::chrono::microseconds(1));
                            (void)co_await delay.wait();
                            stop_src.request_stop();
                        }
                        else
                        {
                            // Write data so read completes normally, then cancel (race!)
                            auto [ec, n] = co_await s1.write_some(
                                capy::const_buffer("x", 1));
                            (void)ec;
                            (void)n;
                            stop_src.request_stop();
                        }

                        // Poll for read to complete (max 1 second)
                        for (int wait = 0; wait < 100; ++wait)
                        {
                            if (read_done.load(std::memory_order_acquire))
                                break;
                            timer t(ioc);
                            t.expires_after(std::chrono::milliseconds(10));
                            (void)co_await t.wait();
                        }

                        if (!read_done.load(std::memory_order_acquire))
                        {
                            std::fprintf(
                                stderr,
                                "  stop_token_stress: read hung on case %d, "
                                "iter %d\n",
                                i % 3, i);
                            BOOST_TEST(
                                read_done.load(std::memory_order_acquire));
                            stop_src.request_stop();
                            timer t(ioc);
                            t.expires_after(std::chrono::milliseconds(100));
                            (void)co_await t.wait();
                        }

                        ++iterations;
                        if (read_ec == capy::cond::canceled)
                            ++cancellations;
                        else if (!read_ec)
                            ++completions;
                    }
                }
                catch (const std::exception& e)
                {
                    std::fprintf(
                        stderr, "  stop_token_stress exception: %s\n",
                        e.what());
                }
            }
        };

        // Timer to stop the test
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);
        };

        capy::run_async(ex)(worker());
        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr,
            "  stop_token_stress: %zu iterations, %zu cancellations, %zu "
            "completions\n",
            iterations.load(), cancellations.load(), completions.load());

        s1.close();
        s2.close();

        BOOST_TEST(iterations.load() > 0);
    }
};

COROSIO_BACKEND_TESTS(
    stop_token_stress_test, "boost.corosio.socket_stress.stop_token")

// Stress Test 2: Synchronous Completion Race (ready_ flag)
//
// This test forces many synchronous completions to stress the
// race between the initiating thread and completion handler thread.

template<auto Backend>
struct sync_completion_stress_test
{
    void run()
    {
        int duration = get_stress_duration();
        std::fprintf(
            stderr, "  sync_completion_stress: running for %d seconds...\n",
            duration);

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Pre-create tcp_socket pair BEFORE ioc.run()
        auto [s1, s2] = make_stress_pair(ioc);

        std::atomic<std::size_t> iterations{0};
        std::atomic<bool> stop_flag{false};

        // Worker: rapid small writes that often complete synchronously
        auto worker = [&]() -> capy::task<> {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                try
                {
                    // Rapid small I/O - these often complete synchronously
                    for (int i = 0;
                         i < 1000 && !stop_flag.load(std::memory_order_relaxed);
                         ++i)
                    {
                        char data = static_cast<char>(i & 0xFF);

                        // Write single byte
                        auto [ec1, n1] = co_await s1.write_some(
                            capy::const_buffer(&data, 1));
                        if (ec1)
                            break;

                        // Read single byte
                        char buf;
                        auto [ec2, n2] = co_await s2.read_some(
                            capy::mutable_buffer(&buf, 1));
                        if (ec2)
                            break;

                        BOOST_TEST_EQ(buf, data);
                        ++iterations;
                    }
                }
                catch (const std::exception& e)
                {
                    std::fprintf(
                        stderr, "  sync_completion_stress exception: %s\n",
                        e.what());
                }
            }
        };

        // Timer to stop the test
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);
        };

        capy::run_async(ex)(worker());
        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr, "  sync_completion_stress: %zu iterations\n",
            iterations.load());

        s1.close();
        s2.close();

        BOOST_TEST(iterations.load() > 0);
    }
};

COROSIO_BACKEND_TESTS(
    sync_completion_stress_test, "boost.corosio.socket_stress.sync_completion")

// Stress Test 3: Rapid Cancel/Close Cycles
//
// This test rapidly cancels and closes sockets to stress the
// cleanup paths and ensure no use-after-free or double-free.

template<auto Backend>
struct cancel_close_stress_test
{
    void run()
    {
        int duration = get_stress_duration();
        std::fprintf(
            stderr, "  cancel_close_stress: running for %d seconds...\n",
            duration);

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Pre-create tcp_socket pair BEFORE ioc.run()
        auto [s1, s2] = make_stress_pair(ioc);

        std::atomic<std::size_t> iterations{0};
        std::atomic<std::size_t> cancels{0};
        std::atomic<std::size_t> writes{0};
        std::atomic<std::size_t> cancel_writes{0};
        std::atomic<bool> stop_flag{false};

        // Worker: rapidly cancel operations on pre-created sockets
        auto worker = [&]() -> capy::task<> {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                try
                {
                    for (int i = 0;
                         i < 50 && !stop_flag.load(std::memory_order_relaxed);
                         ++i)
                    {
                        // Start a blocking read - use atomic for thread-safe signaling
                        char buf[32];
                        std::atomic<bool> read_done{false};
                        std::error_code read_ec;

                        auto read_coro = [&read_done, &read_ec, &s2,
                                          &buf]() -> capy::task<> {
                            auto [ec, n] = co_await s2.read_some(
                                capy::mutable_buffer(buf, sizeof(buf)));
                            read_ec = ec;
                            read_done.store(true, std::memory_order_release);
                        };

                        capy::run_async(ex)(read_coro());

                        // Vary the cancellation method
                        switch (i % 3)
                        {
                        case 0:
                        {
                            // Yield to let the posted read_coro start
                            timer yield_t(ioc);
                            yield_t.expires_after(std::chrono::microseconds(1));
                            (void)co_await yield_t.wait();
                            // Cancel via tcp_socket.cancel()
                            s2.cancel();
                            ++cancels;
                            break;
                        }
                        case 1:
                            // Write data to complete the read normally
                            {
                                auto [ec, n] = co_await s1.write_some(
                                    capy::const_buffer("data", 4));
                                (void)ec;
                                (void)n;
                            }
                            ++writes;
                            break;
                        case 2:
                            // Cancel then immediately write (race)
                            s2.cancel();
                            {
                                auto [ec, n] = co_await s1.write_some(
                                    capy::const_buffer("data", 4));
                                (void)ec;
                                (void)n;
                            }
                            ++cancel_writes;
                            break;
                        }

                        // Poll for read completion with timeout (max 1 second)
                        for (int wait = 0; wait < 100; ++wait)
                        {
                            if (read_done.load(std::memory_order_acquire))
                                break;
                            timer t(ioc);
                            t.expires_after(std::chrono::milliseconds(10));
                            (void)co_await t.wait();
                        }

                        if (!read_done.load(std::memory_order_acquire))
                        {
                            std::fprintf(
                                stderr,
                                "  cancel_close_stress: read hung on case %d, "
                                "iter %d\n",
                                i % 3, i);
                            BOOST_TEST(
                                read_done.load(std::memory_order_acquire));
                            // Force cancel
                            s2.cancel();
                            timer t(ioc);
                            t.expires_after(std::chrono::milliseconds(100));
                            (void)co_await t.wait();
                        }

                        ++iterations;
                    }
                }
                catch (const std::exception& e)
                {
                    std::fprintf(
                        stderr, "  cancel_close_stress exception: %s\n",
                        e.what());
                }
            }
        };

        // Timer to stop the test
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);
        };

        capy::run_async(ex)(worker());
        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr,
            "  cancel_close_stress: %zu iterations (%zu cancels, %zu writes, "
            "%zu cancel+write)\n",
            iterations.load(), cancels.load(), writes.load(),
            cancel_writes.load());

        s1.close();
        s2.close();

        BOOST_TEST(iterations.load() > 0);
    }
};

COROSIO_BACKEND_TESTS(
    cancel_close_stress_test, "boost.corosio.socket_stress.cancel_close")

// Stress Test 4: Concurrent Operations
//
// This test runs multiple concurrent tcp_socket operations to stress
// thread safety and completion dispatch.

template<auto Backend>
struct concurrent_ops_stress_test
{
    void run()
    {
        int duration = get_stress_duration();
        std::fprintf(
            stderr, "  concurrent_ops_stress: running for %d seconds...\n",
            duration);

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        std::atomic<std::size_t> total_bytes{0};
        std::atomic<bool> stop_flag{false};

        constexpr int num_pairs = 4;

        // Create multiple tcp_socket pairs
        std::vector<std::pair<tcp_socket, tcp_socket>> pairs;
        pairs.reserve(num_pairs);
        for (int i = 0; i < num_pairs; ++i)
        {
            pairs.push_back(make_stress_pair(ioc));
        }

        // Writer tasks - use function parameters to pass index reliably
        for (int i = 0; i < num_pairs; ++i)
        {
            capy::run_async(ex)(
                [](tcp_socket& s, std::atomic<bool>& stop,
                   std::atomic<std::size_t>& bytes, int idx) -> capy::task<> {
                    std::size_t sent = 0;
                    char buf[256];
                    std::memset(buf, static_cast<char>(idx), sizeof(buf));

                    while (!stop.load(std::memory_order_relaxed))
                    {
                        auto [ec, n] = co_await s.write_some(
                            capy::const_buffer(buf, sizeof(buf)));
                        if (ec)
                            break;
                        sent += n;
                    }

                    bytes.fetch_add(sent, std::memory_order_relaxed);
                }(pairs[i].first, stop_flag, total_bytes, i));
        }

        // Reader tasks - use function parameters to pass index reliably
        for (int i = 0; i < num_pairs; ++i)
        {
            capy::run_async(ex)(
                [](tcp_socket& s, std::atomic<bool>& stop,
                   std::atomic<std::size_t>& bytes, int) -> capy::task<> {
                    std::size_t received = 0;
                    char buf[256];

                    while (!stop.load(std::memory_order_relaxed))
                    {
                        auto [ec, n] = co_await s.read_some(
                            capy::mutable_buffer(buf, sizeof(buf)));
                        if (ec)
                            break;
                        received += n;
                    }

                    bytes.fetch_add(received, std::memory_order_relaxed);
                }(pairs[i].second, stop_flag, total_bytes, i));
        }

        // Timer to stop the test
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);

            // Close all sockets to unblock pending operations
            for (auto& p : pairs)
            {
                p.first.close();
                p.second.close();
            }
        };

        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr, "  concurrent_ops_stress: %zu total bytes transferred\n",
            total_bytes.load());

        BOOST_TEST(total_bytes.load() > 0);
    }
};

COROSIO_BACKEND_TESTS(
    concurrent_ops_stress_test, "boost.corosio.socket_stress.concurrent_ops")

// Stress Test 5: Accept/Connect Race
//
// This test rapidly accepts and connects to stress the acceptor
// code path and accept completion handling.

template<auto Backend>
struct accept_stress_test
{
    void run()
    {
        int duration = get_stress_duration();
        std::fprintf(
            stderr, "  accept_stress: running for %d seconds...\n", duration);

        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        std::atomic<std::size_t> connections{0};
        std::atomic<bool> stop_flag{false};

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        if (auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0)))
        {
            BOOST_ERROR("accept_stress: bind failed");
            return;
        }
        if (auto ec = acc.listen())
        {
            BOOST_ERROR("accept_stress: listen failed");
            return;
        }
        auto port = acc.local_endpoint().port();

        // Acceptor task
        auto acceptor_task = [&]() -> capy::task<> {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                tcp_socket peer(ioc);
                auto [ec] = co_await acc.accept(peer);
                if (ec)
                {
                    if (stop_flag.load(std::memory_order_relaxed))
                        break;
                    continue;
                }
                ++connections;
                peer.close();
            }
        };

        // Connector task
        auto connector_task = [&]() -> capy::task<> {
            while (!stop_flag.load(std::memory_order_relaxed))
            {
                tcp_socket client(ioc);
                client.open();
                auto [ec] = co_await client.connect(
                    endpoint(ipv4_address::loopback(), port));
                (void)ec;
                client.close();

                // Small delay to avoid overwhelming the accept queue
                timer delay(ioc);
                delay.expires_after(std::chrono::microseconds(100));
                (void)co_await delay.wait();
            }
        };

        capy::run_async(ex)(acceptor_task());
        capy::run_async(ex)(connector_task());

        // Timer to stop the test
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);
            acc.close();
        };

        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr, "  accept_stress: %zu connections\n", connections.load());

        BOOST_TEST(connections.load() > 0);
    }
};

COROSIO_BACKEND_TESTS(accept_stress_test, "boost.corosio.socket_stress.accept")

} // namespace boost::corosio
