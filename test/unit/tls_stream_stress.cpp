//
// Copyright (c) 2026 Vinnie Falco
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Stress tests for OpenSSL and WolfSSL TLS stream adapters.
// These tests hammer TLS-specific code paths to expose race
// conditions, lifetime bugs, and state corruption.
//
// Target areas:
// 1. Session cycling - rapid handshake/data/close lifecycle
// 2. Concurrent TLS I/O - multiple TLS pairs active simultaneously
// 3. Stop token cancellation races during TLS handshake
//
// Tests run for a configurable duration (default 1 second).

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#ifdef BOOST_COROSIO_HAS_OPENSSL
#include <boost/corosio/openssl_stream.hpp>
#endif

#ifdef BOOST_COROSIO_HAS_WOLFSSL
#include <boost/corosio/wolfssl_stream.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stop_token>

#include "test_utils.hpp"
#include "test_suite.hpp"

namespace boost::corosio {
namespace {

constexpr int default_tls_stress_seconds = 1;

int
get_tls_stress_duration()
{
    auto* opt = test_suite::get_command_line_option("stress-duration");
    if (opt)
        return std::atoi(opt);
    return default_tls_stress_seconds;
}

} // namespace

// Stress Test 1: Rapid TLS Session Cycling
//
// Repeatedly creates socket pairs, performs TLS handshake, transfers
// data, and closes. Each iteration exercises the full session
// lifecycle to find state corruption and resource leaks.

template<typename StreamFactory>
struct tls_session_cycle_stress_impl
{
    static constexpr StreamFactory make_stream{};

    void run()
    {
        int duration = get_tls_stress_duration();
        std::fprintf(
            stderr, "  tls_session_cycle: running for %d seconds...\n",
            duration);

        auto stop_time =
            std::chrono::steady_clock::now() + std::chrono::seconds(duration);

        io_context ioc;
        auto ex                = ioc.get_executor();
        std::size_t iterations = 0;

        while (std::chrono::steady_clock::now() < stop_time)
        {
            auto [s1, s2] = corosio::test::make_socket_pair(ioc);

            auto client_ctx = test::make_client_context();
            auto server_ctx = test::make_server_context();

            auto client = make_stream(s1, client_ctx);
            auto server = make_stream(s2, server_ctx);

            // Handshake
            std::error_code cec, sec;

            auto hs_client = [&client, &cec]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                cec       = ec;
            };

            auto hs_server = [&server, &sec]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_stream::server);
                sec       = ec;
            };

            capy::run_async(ex)(hs_client());
            capy::run_async(ex)(hs_server());
            ioc.run();
            ioc.restart();

            BOOST_TEST(!cec);
            BOOST_TEST(!sec);
            if (cec || sec)
            {
                s1.close();
                s2.close();
                continue;
            }

            // Bidirectional data transfer
            auto xfer = [&client, &server]() -> capy::task<> {
                char wbuf[]    = "stress-test-data";
                auto [ec1, n1] = co_await client.write_some(
                    capy::const_buffer(wbuf, sizeof(wbuf) - 1));
                if (ec1)
                    co_return;

                char rbuf[64];
                auto [ec2, n2] = co_await server.read_some(
                    capy::mutable_buffer(rbuf, sizeof(rbuf)));
                BOOST_TEST(!ec2);
                if (!ec2)
                    BOOST_TEST_EQ(n2, sizeof(wbuf) - 1);
            };

            capy::run_async(ex)(xfer());
            ioc.run();
            ioc.restart();

            s1.close();
            s2.close();
            ++iterations;
        }

        std::fprintf(
            stderr, "  tls_session_cycle: %zu sessions completed\n",
            iterations);

        BOOST_TEST(iterations > 0);
    }
};

// Stress Test 2: Concurrent TLS Data Transfer
//
// Two TLS pairs transfer data simultaneously to stress thread
// safety and completion dispatch in the TLS adapter layer.

template<typename StreamFactory>
struct tls_concurrent_io_stress_impl
{
    static constexpr StreamFactory make_stream{};

    void run()
    {
        int duration = get_tls_stress_duration();
        std::fprintf(
            stderr, "  tls_concurrent_io: running for %d seconds...\n",
            duration);

        io_context ioc;
        auto ex = ioc.get_executor();

        // Create two socket pairs
        auto [sa1, sa2] = corosio::test::make_socket_pair(ioc);
        auto [sb1, sb2] = corosio::test::make_socket_pair(ioc);

        auto ca_ctx = test::make_client_context();
        auto sa_ctx = test::make_server_context();
        auto cb_ctx = test::make_client_context();
        auto sb_ctx = test::make_server_context();

        auto client_a = make_stream(sa1, ca_ctx);
        auto server_a = make_stream(sa2, sa_ctx);
        auto client_b = make_stream(sb1, cb_ctx);
        auto server_b = make_stream(sb2, sb_ctx);

        // Handshake pair A
        {
            std::error_code cec, sec;
            auto hsc = [&client_a, &cec]() -> capy::task<> {
                auto [ec] = co_await client_a.handshake(tls_stream::client);
                cec       = ec;
            };
            auto hss = [&server_a, &sec]() -> capy::task<> {
                auto [ec] = co_await server_a.handshake(tls_stream::server);
                sec       = ec;
            };
            capy::run_async(ex)(hsc());
            capy::run_async(ex)(hss());
            ioc.run();
            ioc.restart();
            BOOST_TEST(!cec);
            BOOST_TEST(!sec);
            if (cec || sec)
                return;
        }

        // Handshake pair B
        {
            std::error_code cec, sec;
            auto hsc = [&client_b, &cec]() -> capy::task<> {
                auto [ec] = co_await client_b.handshake(tls_stream::client);
                cec       = ec;
            };
            auto hss = [&server_b, &sec]() -> capy::task<> {
                auto [ec] = co_await server_b.handshake(tls_stream::server);
                sec       = ec;
            };
            capy::run_async(ex)(hsc());
            capy::run_async(ex)(hss());
            ioc.run();
            ioc.restart();
            BOOST_TEST(!cec);
            BOOST_TEST(!sec);
            if (cec || sec)
                return;
        }

        // Concurrent data transfer on both pairs
        std::atomic<std::size_t> total_bytes{0};
        std::atomic<bool> stop_flag{false};

        // Writer: pumps data through a TLS stream until stopped
        auto writer = [](auto& stream, std::atomic<bool>& stop,
                         std::atomic<std::size_t>& bytes) -> capy::task<> {
            char buf[256];
            std::memset(buf, 'W', sizeof(buf));
            std::size_t sent = 0;

            while (!stop.load(std::memory_order_relaxed))
            {
                auto [ec, n] = co_await stream.write_some(
                    capy::const_buffer(buf, sizeof(buf)));
                if (ec)
                    break;
                sent += n;
            }

            bytes.fetch_add(sent, std::memory_order_relaxed);
        };

        // Reader: drains data from a TLS stream until stopped
        auto reader = [](auto& stream, std::atomic<bool>& stop,
                         std::atomic<std::size_t>& bytes) -> capy::task<> {
            char buf[256];
            std::size_t received = 0;

            while (!stop.load(std::memory_order_relaxed))
            {
                auto [ec, n] = co_await stream.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                if (ec)
                    break;
                received += n;
            }

            bytes.fetch_add(received, std::memory_order_relaxed);
        };

        capy::run_async(ex)(writer(client_a, stop_flag, total_bytes));
        capy::run_async(ex)(reader(server_a, stop_flag, total_bytes));
        capy::run_async(ex)(writer(client_b, stop_flag, total_bytes));
        capy::run_async(ex)(reader(server_b, stop_flag, total_bytes));

        // Stopper: wait for duration then close all sockets
        auto stopper = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::seconds(duration));
            (void)co_await t.wait();
            stop_flag.store(true, std::memory_order_relaxed);

            sa1.close();
            sa2.close();
            sb1.close();
            sb2.close();
        };

        capy::run_async(ex)(stopper());

        ioc.run();

        std::fprintf(
            stderr, "  tls_concurrent_io: %zu total bytes transferred\n",
            total_bytes.load());

        BOOST_TEST(total_bytes.load() > 0);
    }
};

// Stress Test 3: TLS Handshake Cancellation Race
//
// Rapidly starts TLS handshakes and cancels them via stop_token
// after the client has sent the ClientHello. Stresses the
// cancellation path in the TLS async state machine.

template<typename StreamFactory>
struct tls_cancel_handshake_stress_impl
{
    static constexpr StreamFactory make_stream{};

    void run()
    {
        int duration = get_tls_stress_duration();
        std::fprintf(
            stderr, "  tls_cancel_handshake: running for %d seconds...\n",
            duration);

        auto stop_time =
            std::chrono::steady_clock::now() + std::chrono::seconds(duration);

        io_context ioc;
        auto ex                   = ioc.get_executor();
        std::size_t iterations    = 0;
        std::size_t cancellations = 0;

        while (std::chrono::steady_clock::now() < stop_time)
        {
            auto [s1, s2] = corosio::test::make_socket_pair(ioc);

            auto client_ctx = test::make_client_context();
            auto server_ctx = test::make_server_context();

            auto client = make_stream(s1, client_ctx);
            auto server = make_stream(s2, server_ctx);

            std::stop_source stop_src;
            bool client_got_error = false;
            bool done             = false;

            // Failsafe to prevent hangs
            timer failsafe(ioc);
            failsafe.expires_after(std::chrono::milliseconds(2000));

            // Client handshake - will be cancelled mid-flight
            auto client_task = [&client, &client_got_error, &done,
                                &failsafe]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_stream::client);
                if (ec)
                    client_got_error = true;
                done = true;
                failsafe.cancel();
            };

            // Server: wait for ClientHello then trigger cancellation
            auto server_task = [&s2, &stop_src]() -> capy::task<> {
                char buf[1];
                (void)co_await s2.read_some(capy::mutable_buffer(buf, 1));
                stop_src.request_stop();
            };

            bool failsafe_hit  = false;
            auto failsafe_task = [&failsafe, &failsafe_hit, &s1,
                                  &s2]() -> capy::task<> {
                auto [ec] = co_await failsafe.wait();
                if (!ec)
                {
                    failsafe_hit = true;
                    if (s1.is_open())
                    {
                        s1.cancel();
                        s1.close();
                    }
                    if (s2.is_open())
                    {
                        s2.cancel();
                        s2.close();
                    }
                }
            };

            capy::run_async(ex, stop_src.get_token())(client_task());
            capy::run_async(ex)(server_task());
            capy::run_async(ex)(failsafe_task());

            ioc.run();
            ioc.restart();

            BOOST_TEST(!failsafe_hit);
            if (client_got_error)
                ++cancellations;

            if (s1.is_open())
                s1.close();
            if (s2.is_open())
                s2.close();
            ++iterations;
        }

        std::fprintf(
            stderr,
            "  tls_cancel_handshake: %zu iterations, %zu cancellations\n",
            iterations, cancellations);

        BOOST_TEST(iterations > 0);
        BOOST_TEST(cancellations > 0);
    }
};

// OpenSSL stress tests

#ifdef BOOST_COROSIO_HAS_OPENSSL

namespace {

struct openssl_stress_factory
{
    auto operator()(tcp_socket& s, tls_context const& ctx) const
    {
        return openssl_stream(&s, ctx);
    }
};

} // namespace

struct openssl_session_cycle_stress
    : tls_session_cycle_stress_impl<openssl_stress_factory>
{};
TEST_SUITE(
    openssl_session_cycle_stress,
    "boost.corosio.tls_stream_stress.openssl.session_cycle");

struct openssl_concurrent_io_stress
    : tls_concurrent_io_stress_impl<openssl_stress_factory>
{};
TEST_SUITE(
    openssl_concurrent_io_stress,
    "boost.corosio.tls_stream_stress.openssl.concurrent_io");

struct openssl_cancel_handshake_stress
    : tls_cancel_handshake_stress_impl<openssl_stress_factory>
{};
TEST_SUITE(
    openssl_cancel_handshake_stress,
    "boost.corosio.tls_stream_stress.openssl.cancel_handshake");

#endif

// WolfSSL stress tests

#ifdef BOOST_COROSIO_HAS_WOLFSSL

namespace {

struct wolfssl_stress_factory
{
    auto operator()(tcp_socket& s, tls_context const& ctx) const
    {
        return wolfssl_stream(&s, ctx);
    }
};

} // namespace

struct wolfssl_session_cycle_stress
    : tls_session_cycle_stress_impl<wolfssl_stress_factory>
{};
TEST_SUITE(
    wolfssl_session_cycle_stress,
    "boost.corosio.tls_stream_stress.wolfssl.session_cycle");

struct wolfssl_concurrent_io_stress
    : tls_concurrent_io_stress_impl<wolfssl_stress_factory>
{};
TEST_SUITE(
    wolfssl_concurrent_io_stress,
    "boost.corosio.tls_stream_stress.wolfssl.concurrent_io");

struct wolfssl_cancel_handshake_stress
    : tls_cancel_handshake_stress_impl<wolfssl_stress_factory>
{};
TEST_SUITE(
    wolfssl_cancel_handshake_stress,
    "boost.corosio.tls_stream_stress.wolfssl.cancel_handshake");

#endif

} // namespace boost::corosio
