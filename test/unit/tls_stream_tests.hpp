//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_TLS_STREAM_TESTS_HPP
#define BOOST_COROSIO_TEST_TLS_STREAM_TESTS_HPP

#include "test_utils.hpp"
#include "stream_tests.hpp"
#include <boost/corosio/delay.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tls_stream.hpp>
#include <boost/corosio/test/mocket.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/capy/test/fuse.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/when_all.hpp>
#include <boost/capy/write.hpp>

#include <array>
#include <cstddef>
#include <stop_token>
#include <thread>
#include <tuple>

#include "test_suite.hpp"

namespace boost::corosio::test {

// Max size variations: small sizes test chunked I/O behavior
inline constexpr std::array<std::size_t, 6> tls_max_sizes = {1,  5,    13,
                                                             64, 1024, 16384};

//
// Fuse Tests - test TLS behavior with chunked I/O
//

/** Test TLS handshake with max_size variations.

    Each max_size variation tests short reads/writes during handshake.
*/
template<typename StreamFactory>
void
testHandshakeFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        // The fuse fails the transport at each successive point; a side
        // that hits an injected error closes both mockets so the peer's
        // parked op completes instead of waiting forever. clean_seen
        // records the injection-free pass, where both handshakes must
        // still succeed.
        bool clean_seen = false;
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            std::error_code client_ec;
            std::error_code server_ec;

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                client_ec = ec;
                if (ec)
                {
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                    m2.close(); // NOLINT(bugprone-unused-return-value)
                }
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                server_ec = ec;
                if (ec)
                {
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                    m2.close(); // NOLINT(bugprone-unused-return-value)
                }
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());

            ioc.run();

            if (!client_ec && !server_ec)
                clean_seen = true;

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
        BOOST_TEST(clean_seen);
    }
}

/** Test TLS read/write with max_size variations.

    After a successful handshake, tests bidirectional data transfer.
    Test data size scales with max_size to keep tests fast.
*/
template<typename StreamFactory>
void
testReadWriteFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            auto test_data = corosio::test::scaled_test_data(max_size);

            // On an injected transport error a side bails and closes
            // both mockets, so the peer's parked op completes at once
            // instead of waiting on bytes that will never come. The
            // data check runs only on the injection-free pass.
            auto bail = [&]() {
                m1.close(); // NOLINT(bugprone-unused-return-value)
                m2.close(); // NOLINT(bugprone-unused-return-value)
            };

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                if (ec)
                    co_return bail();

                auto [ec2, n] = co_await client.write_some(
                    capy::const_buffer(test_data.data(), test_data.size()));
                if (ec2)
                    co_return bail();

                std::string buf(test_data.size(), '\0');
                auto [ec3, n3] = co_await client.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                if (ec3)
                    co_return bail();
                BOOST_TEST(buf.substr(0, n3) == test_data.substr(0, n3));
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                if (ec)
                    co_return bail();

                std::string buf(test_data.size(), '\0');
                auto [ec2, n] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                if (ec2)
                    co_return bail();

                auto [ec3, n3] = co_await server.write_some(
                    capy::const_buffer(buf.data(), n));
                if (ec3)
                    co_return bail();
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());
            ioc.run();

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
    }
}

/** Test TLS shutdown with max_size variations.

    After handshake, tests graceful shutdown.
*/
template<typename StreamFactory>
void
testShutdownFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        // Skip very small max_size for shutdown tests
        // (shutdown is just close_notify, not much data)
        if (max_size < 64)
            continue;

        // On an injected error a side closes both mockets so the peer
        // unblocks. clean_seen records the injection-free pass, where
        // both handshakes must succeed.
        bool clean_seen = false;
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) -> capy::task<> {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            std::error_code client_hs_ec;
            std::error_code server_hs_ec;

            // Each task closes both mockets when it finishes, whatever
            // the outcome: an injected failure mid-shutdown leaves the
            // peer parked on a close_notify that never comes, so the
            // unconditional close is what guarantees no side hangs.
            auto bail = [&]() {
                m1.close(); // NOLINT(bugprone-unused-return-value)
                m2.close(); // NOLINT(bugprone-unused-return-value)
            };

            auto client_task = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                client_hs_ec = ec;
                if (!ec)
                    (void)co_await client.shutdown();
                bail();
            };

            auto server_task = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                server_hs_ec = ec;
                if (!ec)
                {
                    char buf[32];
                    (void)co_await server.read_some(
                        capy::mutable_buffer(buf, sizeof(buf)));
                }
                bail();
            };

            capy::run_async(ioc.get_executor())(client_task());
            capy::run_async(ioc.get_executor())(server_task());
            ioc.run();

            if (!client_hs_ec && !server_hs_ec)
                clean_seen = true;

            m1.close(); // NOLINT(bugprone-unused-return-value)
            co_return;
        });
        BOOST_TEST(clean_seen);
    }
}

//
// Success/Failure Tests
//

/** Test TLS success cases with certificate verification.

    These tests run without fuse injection to verify basic functionality
    works correctly with different certificate configurations.

    @param modes  Array of context_mode values to test
*/
template<typename StreamFactory, std::size_t N>
void
testSuccessCases(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test TLS failure cases.

    Tests various certificate validation failures.
*/
template<typename StreamFactory>
void
testFailureCases(StreamFactory make_stream)
{
    io_context ioc;

    // Client verifies, server has no cert
    {
        auto client_ctx = make_client_context();
        auto server_ctx = make_anon_context();
        std::ignore = server_ctx.set_ciphersuites("");
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
        ioc.restart();
    }

    // Client trusts wrong CA
    {
        auto client_ctx = make_wrong_ca_context();
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
        ioc.restart();
    }
}

/** Test TLS shutdown with proper close_notify exchange. */
template<typename StreamFactory, std::size_t N>
void
testTlsShutdown(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_shutdown_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test stream truncation detection. */
template<typename StreamFactory, std::size_t N>
void
testStreamTruncated(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [client_ctx, server_ctx] = make_contexts(mode);
        run_tls_truncation_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test stop token cancellation. */
template<typename StreamFactory>
void
testStopTokenCancellation(StreamFactory make_stream)
{
    // Cancel during handshake
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_stop_token_handshake_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Cancel during read
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_stop_token_read_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Cancel during write
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_stop_token_write_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test cancellation during shutdown.

    Exercises both cancellation representations (stop token and socket cancel),
    which reach the shutdown read as different error codes; both must surface
    cond::canceled.
*/
template<typename StreamFactory>
void
testShutdownCancel(StreamFactory make_stream)
{
    for (auto mode : {shutdown_cancel_mode::socket_cancel,
                      shutdown_cancel_mode::stop_token})
    {
        io_context ioc;
        auto [client_ctx, server_ctx] =
            make_contexts(context_mode::separate_cert);
        run_shutdown_cancel_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream, mode);
    }
}

/** Test socket error propagation. */
template<typename StreamFactory>
void
testSocketErrorPropagation(StreamFactory make_stream)
{
    // socket.cancel() while TLS blocked on socket I/O
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_socket_cancel_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Connection reset during handshake
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();
        run_connection_reset_test(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** I/O before any handshake completes with an error, never crashes.

    The engines defer session creation to the first handshake; a
    read, write, or shutdown issued before then must surface a code
    through the completion.
*/
template<typename StreamFactory>
void
testIoBeforeHandshake(StreamFactory make_stream)
{
    io_context ioc;
    auto [m, peer] = corosio::test::make_mocket_pair(ioc);

    auto ctx    = make_client_context();
    auto stream = make_stream(m, ctx);

    bool done = false;
    auto task = [&]() -> capy::task<> {
        char buf[16];
        auto [rec, rn] = co_await stream.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        BOOST_TEST(bool(rec));
        BOOST_TEST_EQ(rn, 0u);

        auto [wec, wn] = co_await stream.write_some(
            capy::const_buffer("x", 1));
        BOOST_TEST(bool(wec));
        BOOST_TEST_EQ(wn, 0u);

        auto [sec] = co_await stream.shutdown();
        BOOST_TEST(bool(sec));
        done = true;
    };
    capy::run_async(ioc.get_executor())(task());
    ioc.run();
    BOOST_TEST(done);

    peer.close();
}

/** Test certificate validation. */
template<typename StreamFactory>
void
testCertificateValidation(StreamFactory make_stream)
{
    // Untrusted CA
    {
        io_context ioc;
        auto client_ctx = make_untrusted_ca_client_context();
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Expired certificate
    {
        io_context ioc;
        auto client_ctx = make_expired_client_context();
        auto server_ctx = make_expired_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test SNI (Server Name Indication). */
template<typename StreamFactory>
void
testSni(StreamFactory make_stream)
{
    // Correct hostname succeeds
    {
        io_context ioc;
        auto client_ctx    = make_client_context();
        auto server_ctx    = make_server_context();
        auto with_hostname = [&](auto& s, tls_context const& ctx) {
            auto tls = make_stream(s, ctx);
            tls.set_hostname("www.example.com");
            return tls;
        };
        run_tls_test(
            ioc, client_ctx, server_ctx, with_hostname, make_stream);
    }

    // Wrong hostname fails
    {
        io_context ioc;
        auto client_ctx    = make_client_context();
        auto server_ctx    = make_server_context();
        auto with_hostname = [&](auto& s, tls_context const& ctx) {
            auto tls = make_stream(s, ctx);
            tls.set_hostname("wrong.example.com");
            return tls;
        };
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, with_hostname, make_stream);
    }
}

/** Test SNI callback. */
template<typename StreamFactory>
void
testSniCallback(StreamFactory make_stream)
{
    // SNI callback accepts hostname
    {
        io_context ioc;
        auto client_ctx    = make_client_context();
        auto with_hostname = [&](auto& s, tls_context const& ctx) {
            auto tls = make_stream(s, ctx);
            tls.set_hostname("www.example.com");
            return tls;
        };

        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [](std::string_view hostname) -> bool {
                return hostname == "www.example.com";
            });

        run_tls_test(
            ioc, client_ctx, server_ctx, with_hostname, make_stream);
    }

    // SNI callback rejects hostname
    {
        io_context ioc;
        auto client_ctx    = make_client_context();
        auto with_hostname = [&](auto& s, tls_context const& ctx) {
            auto tls = make_stream(s, ctx);
            tls.set_hostname("www.example.com");
            return tls;
        };

        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [](std::string_view hostname) -> bool {
                return hostname == "api.example.com";
            });

        run_tls_test_fail(
            ioc, client_ctx, server_ctx, with_hostname, make_stream);
    }

    // Client sends no SNI: the server callback is never consulted and
    // the handshake still succeeds (no-acknowledgement, not an error).
    {
        io_context ioc;
        auto client_ctx = make_client_context();

        bool invoked    = false;
        auto server_ctx = make_server_context();
        server_ctx.set_servername_callback(
            [&invoked](std::string_view) -> bool {
                invoked = true;
                return true;
            });

        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(!invoked);
    }
}

namespace hostname_test_detail {

// One handshake round with a clean close_notify exchange so the
// transport is empty for the next round. On handshake failure the
// mockets are closed to unblock the peer.
//
// m1 and m2 are independently typed: make_mocket_pair returns a
// (mocket, peer socket) pair, not two mockets of the same type.
template<typename Stream, typename Mocket1, typename Mocket2>
void
run_hostname_round(
    io_context& ioc,
    Stream& client,
    Stream& server,
    Mocket1& m1,
    Mocket2& m2,
    bool expect_ok)
{
    std::error_code client_ec;
    std::error_code server_ec;

    auto hs_client = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        client_ec = ec;
        if (ec)
        {
            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };
    auto hs_server = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        server_ec = ec;
    };

    capy::run_async(ioc.get_executor())(hs_client());
    capy::run_async(ioc.get_executor())(hs_server());
    ioc.run();
    ioc.restart();

    if (expect_ok)
    {
        BOOST_TEST(!client_ec);
        BOOST_TEST(!server_ec);
        if (client_ec || server_ec)
            return;

        auto sd_client = [&]() -> capy::task<> {
            (void)co_await client.shutdown();
        };
        auto sd_server = [&]() -> capy::task<> {
            char drain[32];
            (void)co_await server.read_some(
                capy::mutable_buffer(drain, sizeof(drain)));
            (void)co_await server.shutdown();
        };
        capy::run_async(ioc.get_executor())(sd_client());
        capy::run_async(ioc.get_executor())(sd_server());
        ioc.run();
        ioc.restart();
    }
    else
    {
        // The server's error after a client-side verification abort is
        // backend- and timing-dependent, so only the client is asserted.
        BOOST_TEST(client_ec != std::error_code());
    }
}

} // namespace hostname_test_detail

/** A hostname set once persists across reset(): SNI is sent on
    every subsequent handshake, not just the first. */
template<typename StreamFactory>
void
testHostnamePersistence(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    std::size_t sni_count = 0;
    server_ctx.set_servername_callback(
        [&sni_count](std::string_view hostname) -> bool {
            if (hostname == "www.example.com")
                ++sni_count;
            return true;
        });

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    client.set_hostname("www.example.com");

    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);
    client.reset();
    server.reset();
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    BOOST_TEST_EQ(sni_count, 2u);

    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** A new hostname set after reset() takes effect on the next
    handshake: the server sees the new SNI name and verification
    is enforced against it. */
template<typename StreamFactory>
void
testHostnameRedirect(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    std::vector<std::string> seen;
    server_ctx.set_servername_callback(
        [&seen](std::string_view hostname) -> bool {
            seen.emplace_back(hostname);
            return true;
        });

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    // Round 1: name matches the certificate
    client.set_hostname("www.example.com");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    client.reset();
    server.reset();

    // Round 2: new name is sent in SNI and fails verification
    // against the old certificate
    client.set_hostname("api.example.com");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, false);

    BOOST_TEST_EQ(seen.size(), 2u);
    if (seen.size() == 2u)
    {
        BOOST_TEST_EQ(seen[0], "www.example.com");
        BOOST_TEST_EQ(seen[1], "api.example.com");
    }

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** set_hostname("") after reset() disables SNI and verification:
    the next handshake sends no SNI, so the servername callback is
    not consulted and the handshake still succeeds. */
template<typename StreamFactory>
void
testHostnameClear(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    std::size_t sni_count = 0;
    server_ctx.set_servername_callback(
        [&sni_count](std::string_view) -> bool {
            ++sni_count;
            return true;
        });

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    client.set_hostname("www.example.com");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    client.reset();
    server.reset();

    client.set_hostname("");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    // Only round 1 sent SNI
    BOOST_TEST_EQ(sni_count, 1u);

    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** A hostname set after a failed handshake attempt takes effect on
    the retry, without an intervening reset(): the failed attempt
    must not pin the old name or the dead session into the next
    handshake. */
template<typename StreamFactory>
void
testHostnameRetryAfterFailure(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    std::vector<std::string> seen;
    server_ctx.set_servername_callback(
        [&seen](std::string_view hostname) -> bool {
            seen.emplace_back(hostname);
            return true;
        });

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    // Round 1: a staged fatal alert record fails the client handshake
    // deterministically, after its hello is already on the wire.
    client.set_hostname("stale.example.com");
    m1.provide(std::string("\x15\x03\x03\x00\x02\x02\x28", 7));

    std::error_code hs_ec;
    auto hs = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        hs_ec = ec;
    };
    capy::run_async(ioc.get_executor())(hs());
    ioc.run();
    ioc.restart();
    BOOST_TEST(hs_ec != std::error_code());

    // Drain the stale hello (one TLS record) from the raw peer socket
    // so the server handshake below starts on a clean transport.
    auto drain = [&]() -> capy::task<> {
        unsigned char hdr[5];
        std::size_t got = 0;
        while (got < sizeof(hdr))
        {
            auto [ec, n] = co_await m2.read_some(capy::mutable_buffer(
                hdr + got, sizeof(hdr) - got));
            if (ec)
                co_return;
            got += n;
        }
        std::vector<char> body(
            (std::size_t(hdr[3]) << 8) | std::size_t(hdr[4]));
        got = 0;
        while (got < body.size())
        {
            auto [ec, n] = co_await m2.read_some(capy::mutable_buffer(
                body.data() + got, body.size() - got));
            if (ec)
                co_return;
            got += n;
        }
    };
    capy::run_async(ioc.get_executor())(drain());
    ioc.run();
    ioc.restart();

    // Round 2: the new name must take effect without reset()
    client.set_hostname("www.example.com");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    // The server only ever saw the retry's SNI; the stale hello was
    // consumed above, before the server stream touched the transport.
    BOOST_TEST_EQ(seen.size(), 1u);
    if (seen.size() == 1u)
        BOOST_TEST_EQ(seen[0], "www.example.com");

    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** An IP-literal hostname is matched against the certificate's
    iPAddress entries and is not sent as SNI; a mismatched literal
    fails verification. On a build that cannot match iPAddress
    entries the handshake fails closed instead. */
template<typename StreamFactory>
void
testHostnameIpLiteral(StreamFactory make_stream, bool ip_supported)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    // NOLINTBEGIN(bugprone-unused-return-value)
    tls_context client_ctx;
    require_ok(client_ctx.add_certificate_authority(test::server_ip_cert_pem));
    require_ok(client_ctx.set_verify_mode(tls_verify_mode::peer));

    tls_context server_ctx;
    require_ok(server_ctx.use_certificate(
        test::server_ip_cert_pem, tls_file_format::pem));
    require_ok(server_ctx.use_private_key(
        test::server_ip_key_pem, tls_file_format::pem));
    require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));
    // NOLINTEND(bugprone-unused-return-value)

    std::size_t sni_count = 0;
    server_ctx.set_servername_callback(
        [&sni_count](std::string_view) -> bool {
            ++sni_count;
            return true;
        });

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    client.set_hostname("127.0.0.1");

    if (!ip_supported)
    {
        // The build cannot parse iPAddress entries; the handshake
        // must fail closed rather than skip verification.
        std::error_code client_ec;
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            client_ec = ec;
            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        };
        auto hs_server = [&]() -> capy::task<> {
            (void)co_await server.handshake(tls_role::server);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();

        BOOST_TEST(client_ec == std::errc::function_not_supported);
        BOOST_TEST_EQ(sni_count, 0u);
        return;
    }

    // Round 1: the literal matches the certificate's iPAddress entry.
    // The CN is not an IP, so success proves the IP-SAN path.
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, true);

    // RFC 6066 excludes literals from SNI
    BOOST_TEST_EQ(sni_count, 0u);

    client.reset();
    server.reset();

    // Round 2: a different literal fails verification
    client.set_hostname("192.0.2.1");
    hostname_test_detail::run_hostname_round(
        ioc, client, server, m1, m2, false);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** A freshly constructed stream reports no negotiated ALPN protocol.

    The accessor must return an empty view before any handshake, on
    every backend (including builds without ALPN support).
*/
template<typename StreamFactory>
void
testAlpnAccessorEmpty(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);
    auto ctx      = make_client_context();
    auto stream   = make_stream(m1, ctx);
    BOOST_TEST(stream.alpn_protocol().empty());
    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Test CRL-based revocation.

    The server presents a leaf that a CRL revokes.

    When @p crl_supported (OpenSSL, or WolfSSL built with HAVE_CRL):
      1. hard_fail with the CRL loaded rejects the revoked leaf.
      2. soft_fail with no CRL loaded accepts (status unknown is allowed).
    Otherwise (WolfSSL without HAVE_CRL): any revocation request fails the
    handshake with function_not_supported rather than skip the check.

    On every build, a CRL that parses as neither PEM nor DER fails the
    handshake closed rather than being silently dropped (which would let
    soft_fail accept a peer the missing CRL might have revoked).
*/
template<typename StreamFactory>
void
testCrlRevocation(StreamFactory make_stream, bool crl_supported)
{
    auto revoked_server = []() {
        tls_context ctx;
        require_ok(ctx.use_certificate(revoked_leaf_cert_pem, tls_file_format::pem));
        require_ok(ctx.use_private_key(revoked_leaf_key_pem, tls_file_format::pem));
        require_ok(ctx.set_verify_mode(tls_verify_mode::none));
        return ctx;
    };
    auto revoking_client = [](tls_revocation_policy policy, bool load_crl) {
        tls_context ctx;
        require_ok(ctx.add_certificate_authority(root_ca_cert_pem));
        require_ok(ctx.set_verify_mode(tls_verify_mode::peer));
        if (load_crl)
            std::ignore = ctx.add_crl(revoked_crl_pem);
        ctx.set_revocation_policy(policy);
        return ctx;
    };

    if (crl_supported)
    {
        // 1. hard_fail + CRL -> revoked leaf rejected.
        {
            io_context ioc;
            auto client_ctx =
                revoking_client(tls_revocation_policy::hard_fail, true);
            auto server_ctx = revoked_server();
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }
        // 2. soft_fail + no CRL -> unknown status accepted.
        {
            io_context ioc;
            auto client_ctx =
                revoking_client(tls_revocation_policy::soft_fail, false);
            auto server_ctx = revoked_server();
            run_tls_test(
                ioc, client_ctx, server_ctx, make_stream, make_stream);
        }
    }
    else
    {
        io_context ioc;
        auto client_ctx =
            revoking_client(tls_revocation_policy::hard_fail, true);
        auto server_ctx = revoked_server();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // A supplied CRL that parses as neither PEM nor DER must fail the
    // handshake, never silently downgrade to accepting the peer. This holds
    // on every build: a HAVE_CRL backend rejects the unparseable CRL, and a
    // backend without CRL support rejects any revocation request outright.
    // Uses soft_fail specifically: without the fail-closed guard, soft_fail
    // would treat the missing (dropped) CRL as "status unknown" and accept.
    {
        io_context ioc;
        tls_context client_ctx;
        require_ok(client_ctx.add_certificate_authority(root_ca_cert_pem));
        require_ok(client_ctx.set_verify_mode(tls_verify_mode::peer));
        std::ignore = client_ctx.add_crl("this is not a valid PEM or DER CRL");
        client_ctx.set_revocation_policy(tls_revocation_policy::soft_fail);
        auto server_ctx = revoked_server();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // A CRL supplied while the policy stays disabled is inert by contract
    // (consulted only when the policy is not disabled). This config must
    // work on every build — including a WolfSSL build without HAVE_CRL,
    // which must not reject a revocation feature that was never requested.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        std::ignore = client_ctx.add_crl(revoked_crl_pem); // policy left disabled
        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test loading server credentials from a PKCS#12 bundle.

    1. A server context whose cert and key come from a PKCS#12 blob
       completes a handshake with a client that trusts the CA.
    2. A wrong passphrase loads no credentials, so the handshake fails.
    3. A bundle takes precedence over discrete cert/key fields (which are
       ignored when a bundle is present).
    4. A client whose bundle fails to decode fails the handshake closed
       rather than silently proceeding with no credential (fail-open mTLS).

    Both backends decode PKCS#12 natively (OpenSSL PKCS12_parse, WolfSSL
    wc_PKCS12_parse), so no capability branching is needed.
*/
template<typename StreamFactory>
void
testPkcs12(StreamFactory make_stream)
{
    std::string_view const p12(
        reinterpret_cast<char const*>(server_p12), sizeof(server_p12));

    // 1. Correct passphrase: credentials load, handshake succeeds.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        require_ok(server_ctx.use_pkcs12(p12, p12_password));
        require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Wrong passphrase: nothing loads, handshake fails.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        require_ok(server_ctx.use_pkcs12(p12, "wrong-password"));
        require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. A PKCS#12 bundle takes precedence over discrete cert/key fields.
    //    The bundle holds a valid, trusted credential; the discrete cert is
    //    an expired self-signed one the client would reject. The handshake
    //    succeeds only if the bundle's credential is used — i.e. the discrete
    //    fields are ignored when a bundle is present.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        tls_context server_ctx;
        require_ok(server_ctx.use_pkcs12(p12, p12_password));
        require_ok(server_ctx.use_certificate(expired_cert_pem, tls_file_format::pem));
        require_ok(server_ctx.use_private_key(expired_key_pem, tls_file_format::pem));
        require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 4. A client whose PKCS#12 credential fails to decode must fail closed,
    //    not silently proceed with no certificate. The server verifies the
    //    peer with `peer` (not `require_peer`), so a client that lost its
    //    identity would otherwise complete the handshake — the fail-open the
    //    review flagged. A wrong passphrase makes the bundle fail to parse.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.use_pkcs12(p12, "wrong-password"));
        auto server_ctx = make_server_context();
        require_ok(server_ctx.set_verify_mode(tls_verify_mode::peer));
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that the intermediate chain inside a PKCS#12 bundle is loaded
    and sent during the handshake.

    The server loads a bundle containing leaf + key + intermediate. The
    client trusts only the root CA, so verification succeeds only if the
    server presents the intermediate — i.e. the bundle's chain was loaded,
    not just the leaf.
*/
template<typename StreamFactory>
void
testPkcs12Chain(StreamFactory make_stream)
{
    std::string_view const p12(
        reinterpret_cast<char const*>(server_chain_p12),
        sizeof(server_chain_p12));

    io_context ioc;
    auto client_ctx = make_rootonly_client_context();
    tls_context server_ctx;
    require_ok(server_ctx.use_pkcs12(p12, p12_password));
    require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));
    run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Certificate-chain delivery: the server must send its intermediate.

    Both backends load a full chain (leaf + intermediate) and send the
    intermediate during the handshake, so a client that trusts only the
    root CA can complete the path. A server that presents only its leaf
    cannot, since the client lacks the intermediate to bridge to the root.

    1. Server sends the full chain, client trusts only the root -> succeeds.
    2. Server sends the leaf only, client trusts only the root -> fails.

    The intermediate carries a critical basicConstraints extension so that
    WolfSSL (which enforces RFC 5280 strictly) accepts it as a CA, matching
    OpenSSL.
*/
template<typename StreamFactory>
void
testCertificateChain(StreamFactory make_stream)
{
    // Server sends the full chain; client trusting only the root succeeds.
    {
        io_context ioc;
        auto client_ctx = make_rootonly_client_context();
        auto server_ctx = make_fullchain_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Server sends only the leaf; the client cannot build the path -> fails.
    {
        io_context ioc;
        auto client_ctx = make_rootonly_client_context();
        auto server_ctx = make_chain_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** set_default_verify_paths() applies cleanly on top of an explicit CA.

    Both backends can add the system trust store (OpenSSL:
    SSL_CTX_set_default_verify_paths; WolfSSL: wolfSSL_CTX_load_system_CA_certs
    where the build enables it, otherwise a no-op). Behaviorally exercising
    the system store would require redirecting it per-platform, which is not
    portable across the CI matrix, so this asserts only that the call path
    runs cleanly: a client trusting the test CA explicitly *and* adding the
    system store still completes the handshake. The load-from-path mechanism
    is covered behaviorally by each backend's add_verify_path test.
*/
template<typename StreamFactory>
void
testDefaultVerifyPaths(StreamFactory make_stream)
{
    io_context ioc;
    auto client_ctx = make_client_context();
    // Adding the system store on top of the explicit CA must not break
    // context creation or verification.
    require_ok(client_ctx.set_default_verify_paths());

    auto server_ctx = make_server_context();
    run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Test that TLS 1.3 cipher suite selection is applied.

    1. Both peers restricted to @p suite_a -> handshake succeeds.
    2. Client restricted to @p suite_a, server to @p suite_b (disjoint)
       -> no common suite, handshake fails.
    3. An unparseable cipher string fails the handshake closed (setup_failed_)
       rather than silently reverting to the default suites.

    Suite names differ between backends (OpenSSL `TLS_AES_128_GCM_SHA256`
    vs WolfSSL `TLS13-AES128-GCM-SHA256`), so the caller supplies them.
*/
template<typename StreamFactory>
void
testCiphersuitesTls13(
    StreamFactory make_stream, char const* suite_a, char const* suite_b)
{
    auto make_ctx = [&](auto base, char const* suite) {
        auto ctx = base();
        require_ok(ctx.set_min_protocol_version(tls_version::tls_1_3));
        require_ok(ctx.set_ciphersuites_tls13(suite));
        return ctx;
    };

    // 1. Matching suite succeeds.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, suite_a);
        auto server_ctx = make_ctx(make_server_context, suite_a);
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Disjoint suites fail.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, suite_a);
        auto server_ctx = make_ctx(make_server_context, suite_b);
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. A cipher string the library rejects fails closed, rather than
    //    silently falling back to the default suites.
    {
        io_context ioc;
        auto client_ctx = make_ctx(make_client_context, "not-a-real-suite");
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that protocol version bounds are enforced.

    1. Both peers pinned to TLS 1.3 -> handshake succeeds.
    2. Client capped at TLS 1.2 while the server requires TLS 1.3 ->
       no common version, handshake fails.

    Both backends support version bounds natively, so no capability
    branching is needed.
*/
template<typename StreamFactory>
void
testProtocolVersion(StreamFactory make_stream)
{
    // 1. TLS 1.3 on both sides succeeds.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_min_protocol_version(tls_version::tls_1_3));
        auto server_ctx = make_server_context();
        require_ok(server_ctx.set_min_protocol_version(tls_version::tls_1_3));
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 2. Version window mismatch fails: client max 1.2, server min 1.3.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_max_protocol_version(tls_version::tls_1_2));
        auto server_ctx = make_server_context();
        require_ok(server_ctx.set_min_protocol_version(tls_version::tls_1_3));
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // 3. An inverted window on one context (min 1.3 > max 1.2) admits no
    //    protocol; the handshake must fail closed rather than silently
    //    negotiate an unexpected version.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_min_protocol_version(tls_version::tls_1_3));
        require_ok(client_ctx.set_max_protocol_version(tls_version::tls_1_2));
        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test ALPN negotiation end-to-end.

    Client and server both offer `{ "h2", "http/1.1" }`.

    @param alpn_supported Whether the backend build can negotiate ALPN.
        When true, the handshake succeeds and both peers report `"h2"`.
        When false (e.g. WolfSSL without `HAVE_ALPN`), offering ALPN must
        fail the handshake with `function_not_supported` rather than
        silently negotiate nothing.
*/
template<typename StreamFactory>
void
testAlpn(StreamFactory make_stream, bool alpn_supported)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    require_ok(client_ctx.set_alpn({"h2", "http/1.1"}));
    auto server_ctx = make_server_context();
    require_ok(server_ctx.set_alpn({"h2", "http/1.1"}));

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    std::error_code cec, sec;
    auto hc = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        cec       = ec;
    };
    auto hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        sec       = ec;
    };
    capy::run_async(ioc.get_executor())(hc());
    capy::run_async(ioc.get_executor())(hs());
    ioc.run();

    if (alpn_supported)
    {
        BOOST_TEST(!cec);
        BOOST_TEST(!sec);
        BOOST_TEST(client.alpn_protocol() == "h2");
        BOOST_TEST(server.alpn_protocol() == "h2");
    }
    else
    {
        // Fail-closed: offering ALPN a build cannot honor must not
        // silently proceed.
        BOOST_TEST(
            cec == std::errc::function_not_supported ||
            sec == std::errc::function_not_supported);
    }

    m1.close(); // NOLINT(bugprone-unused-return-value)
    m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** ALPN with no common protocol fails the handshake (RFC 7301 §3.2).

    Client offers `{"h2"}`, server offers `{"http/1.1"}`. A server that
    supports ALPN but shares no protocol with the client must abort with a
    fatal `no_application_protocol` alert, so both peers see the handshake
    fail.

    @param alpn_supported Whether the backend build can negotiate ALPN. When
        false (e.g. WolfSSL without `HAVE_ALPN`) offering ALPN already fails
        closed regardless of overlap — that path is covered by @ref testAlpn —
        so this test only runs when ALPN is available.
*/
template<typename StreamFactory>
void
testAlpnNoOverlap(StreamFactory make_stream, bool alpn_supported)
{
    if (!alpn_supported)
        return;

    io_context ioc;
    auto client_ctx = make_client_context();
    require_ok(client_ctx.set_alpn({"h2"}));
    auto server_ctx = make_server_context();
    require_ok(server_ctx.set_alpn({"http/1.1"}));
    run_tls_test_fail(ioc, client_ctx, server_ctx, make_stream, make_stream);
}

/** Test the certificate verification callback (portable contract).

    Exercises the guarantees that hold on every backend that supports the
    callback: it is consulted when the backend's built-in verification
    fails, its return value determines the outcome, and it can inspect the
    certificate's DER portably.

      1. Override accept: an untrusted CA would normally fail, but a
         callback returning true completes the handshake, and the
         callback observes preverified == false with a usable native
         handle and DER bytes.
      2. Decline: an untrusted CA with a callback returning false leaves
         the handshake failed.

    @param callback_supported Whether this backend build can honor a
        verify callback. Pass `false` for builds that cannot invoke the
        callback on a successful handshake (e.g. WolfSSL without
        WOLFSSL_ALWAYS_VERIFY_CB): installing a callback must then fail
        the handshake with `std::errc::function_not_supported` rather than
        silently ignore it (which would let a tightening callback fail
        open).
*/
template<typename StreamFactory>
void
testVerifyCallback(StreamFactory make_stream, bool callback_supported = true)
{
    if (!callback_supported)
    {
        // Fail-closed: a context carrying a verify_callback must fail the
        // handshake with a clear error, never silently accept.
        {
            io_context ioc;
            auto client_ctx = make_client_context();
            require_ok(client_ctx.set_verify_callback(
                [](bool preverified, verify_context&) -> bool {
                    return preverified;
                }));
            auto server_ctx = make_server_context();
            std::error_code client_ec;
            run_tls_test_fail(
                ioc, client_ctx, server_ctx, make_stream, make_stream,
                &client_ec);
            BOOST_TEST(client_ec == std::errc::function_not_supported);
        }

        // A retried handshake on the same stream must stay fail-closed. The
        // fail-closed path returns before any I/O, so this needs no peer;
        // it is a regression guard against leaving the native session
        // non-null (which would let the retry bypass the check).
        {
            io_context ioc;
            auto [m1, m2] = corosio::test::make_mocket_pair(ioc);
            (void)m2;
            auto client_ctx = make_client_context();
            require_ok(client_ctx.set_verify_callback(
                [](bool preverified, verify_context&) -> bool {
                    return preverified;
                }));
            auto client = make_stream(m1, client_ctx);

            std::error_code ec1;
            std::error_code ec2;
            auto attempt = [&](std::error_code& out) -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                out = ec;
            };
            capy::run_async(ioc.get_executor())(attempt(ec1));
            ioc.run();
            ioc.restart();
            capy::run_async(ioc.get_executor())(attempt(ec2));
            ioc.run();

            BOOST_TEST(ec1 == std::errc::function_not_supported);
            BOOST_TEST(ec2 == std::errc::function_not_supported);

            m1.close(); // NOLINT(bugprone-unused-return-value)
        }
        return;
    }

    // 1. Override accept: untrusted CA (preverified == false) but callback
    //    returns true -> handshake succeeds. Without the callback this
    //    configuration fails, so success proves the callback is consulted
    //    and its return value controls the result.
    {
        io_context ioc;
        bool saw_unverified = false;

        auto client_ctx = make_wrong_ca_context();
        require_ok(client_ctx.set_verify_callback(
            [&saw_unverified](bool preverified, verify_context& vc) -> bool {
                if (!preverified)
                {
                    saw_unverified = true;
                    BOOST_TEST(vc.native_handle() != nullptr);
                    // Portable certificate access: the callback must see the
                    // DER bytes of the cert under verification on both
                    // backends. A DER certificate is an ASN.1 SEQUENCE, so
                    // the first byte is 0x30.
                    auto der = vc.certificate();
                    BOOST_TEST(!der.empty());
                    if (!der.empty())
                        BOOST_TEST(der[0] == 0x30);
                }
                return true;
            }));

        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(saw_unverified);
    }

    // 2. Decline: untrusted CA and callback returns false -> stays failed.
    {
        io_context ioc;
        auto client_ctx = make_wrong_ca_context();
        require_ok(client_ctx.set_verify_callback(
            [](bool, verify_context&) -> bool { return false; }));

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test that the verify callback also runs on successful verification.

    Backends that support the full callback contract (OpenSSL always;
    WolfSSL with WOLFSSL_ALWAYS_VERIFY_CB) invoke it for a certificate that
    passed the built-in checks, which lets a callback reject an otherwise
    valid certificate (e.g. for pinning) and inspect it on the success
    path. Only call this for backends where the callback is fully
    supported.
*/
template<typename StreamFactory>
void
testVerifyCallbackOnSuccess(StreamFactory make_stream)
{
    // Invoked on success: trusted CA + pass-through callback succeeds, and
    // the certificate is inspectable on the success path.
    {
        io_context ioc;
        bool invoked  = false;
        bool saw_cert = false;

        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_verify_callback(
            [&](bool preverified, verify_context& vc) -> bool {
                invoked = true;
                if (preverified && !vc.certificate().empty() &&
                    vc.certificate()[0] == 0x30)
                    saw_cert = true;
                return preverified;
            }));

        auto server_ctx = make_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        BOOST_TEST(invoked);
        BOOST_TEST(saw_cert);
    }

    // Reject a valid cert: trusted CA but callback returns false -> fail.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_verify_callback(
            [](bool, verify_context&) -> bool { return false; }));

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // Content-based pinning: reject unless the leaf DER matches an expected
    // pin. A deliberately-wrong pin must fail the handshake even though the
    // chain is otherwise valid.
    {
        io_context ioc;
        auto client_ctx = make_client_context();
        require_ok(client_ctx.set_verify_callback(
            [](bool preverified, verify_context& vc) -> bool {
                if (!preverified)
                    return false;
                auto der = vc.certificate();
                return der.size() == 1 && der[0] == 0xFF; // never matches
            }));

        auto server_ctx = make_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

/** Test move construction and move assignment of a live stream.

    The moved-into stream must keep working after a completed
    handshake; the moved-from stream must be safely destroyable.
*/
template<typename StreamFactory>
void
testMoveSemantics(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client_pre = make_stream(m1, client_ctx);
    auto client_orig{std::move(client_pre)};
    auto server = make_stream(m2, server_ctx);

    auto client_hs = [&]() -> capy::task<> {
        auto [ec] = co_await client_orig.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();
    ioc.restart();

    // Move-construct from the handshaked stream, then move-assign
    // over a freshly constructed target (its impl must be released).
    auto client_moved = std::move(client_orig);
    auto client       = make_stream(m1, client_ctx);
    client            = std::move(client_moved);

    bool write_done = false, read_done = false;
    auto writer = [&]() -> capy::task<> {
        auto [ec, n] = co_await client.write_some(
            capy::const_buffer("moved", 5));
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);
        write_done = true;
    };
    auto reader = [&]() -> capy::task<> {
        char buf[16];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 5u);
        read_done = true;
    };
    capy::run_async(ioc.get_executor())(writer());
    capy::run_async(ioc.get_executor())(reader());
    ioc.run();

    BOOST_TEST(write_done);
    BOOST_TEST(read_done);
}

/** Test reads and shutdown after the peer vanishes without close_notify.

    The transport closing under an established TLS session must surface
    `stream_truncated` to a pending read, and a subsequent shutdown()
    must complete instead of hanging.
*/
template<typename StreamFactory>
void
testAbruptClose(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    auto client_hs = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();
    ioc.restart();

    // Server's transport disappears without a TLS shutdown.
    m2.close();

    bool read_done = false;
    std::error_code read_ec;
    auto reader = [&]() -> capy::task<> {
        char buf[16];
        auto [ec, n] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        (void)n;
        read_ec   = ec;
        read_done = true;
    };
    capy::run_async(ioc.get_executor())(reader());
    ioc.run();
    ioc.restart();

    BOOST_TEST(read_done);
    BOOST_TEST(read_ec == capy::error::stream_truncated);

    // Shutdown must complete; the missing close_notify reply is
    // normalized, not reported as a transport error.
    bool shutdown_done = false;
    auto closer = [&]() -> capy::task<> {
        (void)co_await client.shutdown();
        shutdown_done = true;
    };
    capy::run_async(ioc.get_executor())(closer());
    ioc.run();

    BOOST_TEST(shutdown_done);
}

/** Test handshaking with a password-protected server key.

    The server private key is encrypted; loading it must route through
    the context's password callback.

    @param expect_success Whether the handshake must succeed. Pass
        `false` for TLS builds whose encrypted-key support is a
        compile-time feature (wolfSSL): the key-loading path and the
        password callback still run, but the handshake may fail
        cleanly instead of completing.
*/
template<typename StreamFactory>
void
testEncryptedKey(StreamFactory make_stream, bool expect_success = true)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx       = make_client_context();
    bool callback_invoked = false;
    auto server_ctx = make_encrypted_key_server_context(callback_invoked);

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    bool client_done = false, server_done = false;
    bool failsafe_hit = false;
    std::error_code client_ec, server_ec;

    // If the TLS build cannot decrypt the key the handshake stalls
    // with both sides waiting; tear the transport down instead of
    // hanging the suite.
    std::stop_source failsafe_stop;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await client.handshake(tls_role::client);
        client_ec   = ec;
        client_done = true;
        if (server_done)
            failsafe_stop.request_stop();
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await server.handshake(tls_role::server);
        server_ec   = ec;
        server_done = true;
        if (client_done)
            failsafe_stop.request_stop();
    };
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(std::chrono::seconds(5));
        if (!ec)
        {
            failsafe_hit = true;
            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(client_done);
    BOOST_TEST(server_done);
    BOOST_TEST(callback_invoked);
    if (expect_success)
    {
        BOOST_TEST(!failsafe_hit);
        BOOST_TEST(!client_ec);
        BOOST_TEST(!server_ec);
    }
}

/** Test that a context with unparseable credentials fails the
    handshake with an error instead of crashing or hanging.
*/
template<typename StreamFactory>
void
testInvalidContextHandshake(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();

    tls_context server_ctx;
    // The setters may reject the garbage eagerly or defer to the
    // handshake; the handshake failure below is what is asserted.
    std::ignore = server_ctx.use_certificate("not a certificate", tls_file_format::pem);
    std::ignore = server_ctx.use_private_key("not a key", tls_file_format::pem);
    require_ok(server_ctx.set_verify_mode(tls_verify_mode::none));

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    bool client_done = false, server_done = false;
    std::error_code client_ec, server_ec;

    auto client_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await client.handshake(tls_role::client);
        client_ec   = ec;
        client_done = true;
        // Unblock the server if it is still waiting on the transport.
        m1.close(); // NOLINT(bugprone-unused-return-value)
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec]   = co_await server.handshake(tls_role::server);
        server_ec   = ec;
        server_done = true;
        m2.close(); // NOLINT(bugprone-unused-return-value)
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();

    BOOST_TEST(client_done);
    BOOST_TEST(server_done);
    // At least one side must report the failure.
    BOOST_TEST(!!client_ec || !!server_ec);
}

/** Test mutual TLS (mTLS). */
template<typename StreamFactory>
void
testMtls(StreamFactory make_stream)
{
    // mTLS success
    {
        io_context ioc;
        auto client_ctx = make_mtls_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // mTLS failure - no client cert
    {
        io_context ioc;
        auto client_ctx = make_chain_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }

    // mTLS failure - wrong client cert
    {
        io_context ioc;
        auto client_ctx = make_invalid_mtls_client_context();
        auto server_ctx = make_mtls_server_context();
        run_tls_test_fail(
            ioc, client_ctx, server_ctx, make_stream, make_stream);
    }
}

//
// Reset Tests
//

/** Test explicit reset() between TLS sessions.

    Verifies that calling reset() after shutdown allows the
    same stream objects to perform a new handshake and data
    transfer. Two full rounds on the same stream pair.
*/
template<typename StreamFactory, std::size_t N>
void
testReset(StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto [client_ctx, server_ctx] = make_contexts(mode);
        auto client                   = make_stream(m1, client_ctx);
        auto server                   = make_stream(m2, server_ctx);

        auto do_round = [&](std::string const& msg) {
            std::error_code client_ec;
            std::error_code server_ec;

            // Handshake
            auto hs_client = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                client_ec = ec;
            };
            auto hs_server = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                server_ec = ec;
            };

            capy::run_async(ioc.get_executor())(hs_client());
            capy::run_async(ioc.get_executor())(hs_server());
            ioc.run();
            ioc.restart();

            BOOST_TEST(!client_ec);
            BOOST_TEST(!server_ec);
            if (client_ec || server_ec)
                return;

            // Data transfer
            auto xfer = [&]() -> capy::task<> {
                // Client writes
                auto [wec, wn] = co_await client.write_some(
                    capy::const_buffer(msg.data(), msg.size()));
                BOOST_TEST(!wec);
                if (wec)
                    co_return;

                // Server reads
                std::string buf(msg.size(), '\0');
                auto [rec, rn] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!rec);
                if (!rec)
                    BOOST_TEST(buf.substr(0, rn) == msg.substr(0, rn));
            };
            capy::run_async(ioc.get_executor())(xfer());
            ioc.run();
            ioc.restart();

            // Shutdown both sides concurrently
            auto sd_client = [&]() -> capy::task<> {
                (void)co_await client.shutdown();
            };
            auto sd_server = [&]() -> capy::task<> {
                // Read until close_notify, then send ours
                char drain[32];
                (void)co_await server.read_some(
                    capy::mutable_buffer(drain, sizeof(drain)));
                (void)co_await server.shutdown();
            };

            capy::run_async(ioc.get_executor())(sd_client());
            capy::run_async(ioc.get_executor())(sd_server());
            ioc.run();
            ioc.restart();
        };

        // Round 1
        do_round("hello1");

        // Explicit reset
        client.reset();
        server.reset();

        // Round 2
        do_round("hello2");

        m1.close(); // NOLINT(bugprone-unused-return-value)
        m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Test implicit reset via handshake().

    Verifies that calling handshake() on a previously-used stream
    automatically resets, without an explicit reset() call.
*/
template<typename StreamFactory, std::size_t N>
void
testResetViaHandshake(
    StreamFactory make_stream, std::array<context_mode, N> const& modes)
{
    for (auto mode : modes)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto [client_ctx, server_ctx] = make_contexts(mode);
        auto client                   = make_stream(m1, client_ctx);
        auto server                   = make_stream(m2, server_ctx);

        auto do_round = [&](std::string const& msg) {
            std::error_code client_ec;
            std::error_code server_ec;

            auto hs_client = [&]() -> capy::task<> {
                auto [ec] = co_await client.handshake(tls_role::client);
                client_ec = ec;
            };
            auto hs_server = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                server_ec = ec;
            };

            capy::run_async(ioc.get_executor())(hs_client());
            capy::run_async(ioc.get_executor())(hs_server());
            ioc.run();
            ioc.restart();

            BOOST_TEST(!client_ec);
            BOOST_TEST(!server_ec);
            if (client_ec || server_ec)
                return;

            auto xfer = [&]() -> capy::task<> {
                auto [wec, wn] = co_await client.write_some(
                    capy::const_buffer(msg.data(), msg.size()));
                BOOST_TEST(!wec);
                if (wec)
                    co_return;

                std::string buf(msg.size(), '\0');
                auto [rec, rn] = co_await server.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                BOOST_TEST(!rec);
                if (!rec)
                    BOOST_TEST(buf.substr(0, rn) == msg.substr(0, rn));
            };
            capy::run_async(ioc.get_executor())(xfer());
            ioc.run();
            ioc.restart();

            auto sd_client = [&]() -> capy::task<> {
                (void)co_await client.shutdown();
            };
            auto sd_server = [&]() -> capy::task<> {
                char drain[32];
                (void)co_await server.read_some(
                    capy::mutable_buffer(drain, sizeof(drain)));
                (void)co_await server.shutdown();
            };

            capy::run_async(ioc.get_executor())(sd_client());
            capy::run_async(ioc.get_executor())(sd_server());
            ioc.run();
            ioc.restart();
        };

        // Round 1
        do_round("round1");

        // No explicit reset -- handshake() should auto-reset

        // Round 2
        do_round("round2");

        m1.close(); // NOLINT(bugprone-unused-return-value)
        m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Test reset with fuse/max_size variations.

    Stresses chunked I/O across reset boundaries.
*/
template<typename StreamFactory>
void
testResetFuse(StreamFactory make_stream)
{
    for (auto max_size : tls_max_sizes)
    {
        if (max_size < 64)
            continue;

        // A side hitting an injected error closes both mockets so the
        // peer unblocks, and the round returns early so the body never
        // proceeds to reset/round-2 on a broken session. clean_seen
        // records the injection-free pass, which must complete both
        // rounds' handshakes across a reset.
        bool clean_seen = false;
        capy::test::fuse f;
        f.armed([&](capy::test::fuse&) {
            io_context ioc;
            auto [m1, m2] =
                corosio::test::make_mocket_pair(ioc, f, max_size, max_size);

            auto client_ctx = make_client_context();
            auto server_ctx = make_server_context();

            auto client = make_stream(m1, client_ctx);
            auto server = make_stream(m2, server_ctx);

            auto bail = [&]() {
                m1.close(); // NOLINT(bugprone-unused-return-value)
                m2.close(); // NOLINT(bugprone-unused-return-value)
            };

            // Round 1
            {
                std::error_code cec, sec;
                auto hsc = [&]() -> capy::task<> {
                    auto [ec] = co_await client.handshake(tls_role::client);
                    cec       = ec;
                    if (ec)
                        bail();
                };
                auto hss = [&]() -> capy::task<> {
                    auto [ec] = co_await server.handshake(tls_role::server);
                    sec       = ec;
                    if (ec)
                        bail();
                };
                capy::run_async(ioc.get_executor())(hsc());
                capy::run_async(ioc.get_executor())(hss());
                ioc.run();
                ioc.restart();
                if (cec || sec)
                    return;

                // Shutdown. On an injected error a side closes both
                // mockets so the peer's parked close_notify wait ends;
                // sd_failed then skips reset/round-2, which need both
                // mockets intact.
                bool sd_failed = false;
                auto sdc       = [&]() -> capy::task<> {
                    auto [ec] = co_await client.shutdown();
                    if (ec)
                    {
                        sd_failed = true;
                        bail();
                    }
                };
                auto sds = [&]() -> capy::task<> {
                    char drain[32];
                    auto [rec, rn] = co_await server.read_some(
                        capy::mutable_buffer(drain, sizeof(drain)));
                    if (rec && rec != capy::cond::eof)
                    {
                        sd_failed = true;
                        bail();
                        co_return;
                    }
                    auto [ec] = co_await server.shutdown();
                    if (ec)
                    {
                        sd_failed = true;
                        bail();
                    }
                };
                capy::run_async(ioc.get_executor())(sdc());
                capy::run_async(ioc.get_executor())(sds());
                ioc.run();
                ioc.restart();
                if (sd_failed)
                    return;
            }

            // Reset both
            client.reset();
            server.reset();

            // Round 2
            {
                std::error_code cec, sec;
                auto hsc = [&]() -> capy::task<> {
                    auto [ec] = co_await client.handshake(tls_role::client);
                    cec       = ec;
                    if (ec)
                        bail();
                };
                auto hss = [&]() -> capy::task<> {
                    auto [ec] = co_await server.handshake(tls_role::server);
                    sec       = ec;
                    if (ec)
                        bail();
                };
                capy::run_async(ioc.get_executor())(hsc());
                capy::run_async(ioc.get_executor())(hss());
                ioc.run();
                ioc.restart();
                if (!cec && !sec)
                    clean_seen = true;
            }

            m1.close(); // NOLINT(bugprone-unused-return-value)
            m2.close(); // NOLINT(bugprone-unused-return-value)
        });
        BOOST_TEST(clean_seen);
    }
}

/** Concurrent read and write on one stream make progress together:
    a reader parked on the transport must not block a writer's flush. */
template<typename StreamFactory>
void
testFullDuplex(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    // Handshake phase
    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    // Session phase: the server reads (peer sends nothing more) while
    // concurrently writing a reply the client is waiting for.
    std::string const request = "request payload";
    std::string const reply(4096, 'r');
    bool server_done  = false;
    bool client_done  = false;
    bool failsafe_hit = false;
    std::stop_source failsafe_stop;

    auto server_reader = [&]() -> capy::io_task<> {
        char buf[4096];
        for (;;)
        {
            auto [ec, n] = co_await server.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            if (ec)
                co_return {ec};
        }
    };
    auto server_writer = [&]() -> capy::io_task<> {
        auto [ec, n] = co_await capy::write(
            server, capy::const_buffer(reply.data(), reply.size()));
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, reply.size());
        co_return {ec};
    };
    auto server_task = [&]() -> capy::task<> {
        // Consume the request before starting the concurrent pair so
        // the reader parks with nothing left in flight; the failure
        // mode then reproduces regardless of completion ordering.
        char req[4096];
        auto [rec, rn] = co_await server.read_some(
            capy::mutable_buffer(req, sizeof(req)));
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(
            std::string_view(req, rn), std::string_view(request));

        std::ignore =
            co_await capy::when_all(server_reader(), server_writer());
        server_done = true;
        failsafe_stop.request_stop();
    };

    auto client_task = [&]() -> capy::task<> {
        auto [wec, wn] = co_await capy::write(
            client, capy::const_buffer(request.data(), request.size()));
        BOOST_TEST(!wec);

        std::string got;
        char buf[4096];
        while (got.size() < reply.size())
        {
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            if (ec)
                break;
            got.append(buf, n);
        }
        BOOST_TEST_EQ(got, reply);
        client_done = true;

        // Tear down the transport so the server's reader completes.
        m1.close(); // NOLINT(bugprone-unused-return-value)
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(client_done);
    BOOST_TEST(server_done);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Sustained transfer in both directions at once: claim interleaving,
    record ordering, and leftover-input handling under load. */
template<typename StreamFactory>
void
testFullDuplexBulk(StreamFactory make_stream)
{
    constexpr std::size_t chunk_size  = std::size_t{16} * 1024;
    constexpr std::size_t chunk_count = 32;
    constexpr std::size_t total       = chunk_size * chunk_count;

    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    // Each side sends `total` bytes of a distinct pattern while reading
    // the peer's stream to completion.
    auto pump = [](auto& stream, char send_fill, char expect_fill,
                   bool& ok) -> capy::task<>
    {
        std::string const chunk(chunk_size, send_fill);
        std::size_t sent = 0, received = 0;
        bool order_ok = true;

        auto sender = [&]() -> capy::io_task<> {
            while (sent < total)
            {
                auto [ec, n] = co_await capy::write(
                    stream,
                    capy::const_buffer(chunk.data(), chunk.size()));
                if (ec)
                    co_return {ec};
                sent += n;
            }
            co_return {std::error_code{}};
        };
        auto receiver = [&]() -> capy::io_task<> {
            char buf[chunk_size];
            while (received < total)
            {
                auto [ec, n] = co_await stream.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                if (ec)
                    co_return {ec};
                for (std::size_t i = 0; i < n; ++i)
                    if (buf[i] != expect_fill)
                        order_ok = false;
                received += n;
            }
            co_return {std::error_code{}};
        };

        std::ignore = co_await capy::when_all(sender(), receiver());
        ok = order_ok && sent == total && received == total;
    };

    bool client_ok = false, server_ok = false, failsafe_hit = false;
    std::stop_source failsafe_stop;

    auto client_task = [&]() -> capy::task<> {
        co_await pump(client, 'c', 's', client_ok);
    };
    auto server_task = [&]() -> capy::task<> {
        co_await pump(server, 's', 'c', server_ok);
        failsafe_stop.request_stop();
    };
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(5000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(client_ok);
    BOOST_TEST(server_ok);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Exact TLS record-boundary payloads: one transfer of exactly 16384
    bytes (the maximum plaintext record size) and one of 16385 (the
    first size that must split across two records), in both
    directions. Confirms byte-exact delivery right at the boundary,
    where testFullDuplexBulk's fixed 16 KiB chunking never lands.
*/
template<typename StreamFactory>
void
testRecordBoundaryTransfer(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    // One exact-size transfer: the sender loops write_some (via
    // capy::write) until every byte is handed to the engine — the
    // 16385 case forces that loop, since a single write_some cannot
    // exceed one TLS record's plaintext capacity.
    auto transfer = [](auto& from, auto& to, std::size_t size,
                       char fill) -> capy::task<> {
        std::string const payload(size, fill);
        std::string got;

        auto sender = [&]() -> capy::io_task<> {
            auto [ec, n] = co_await capy::write(
                from, capy::const_buffer(payload.data(), payload.size()));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, size);
            co_return {ec};
        };
        auto receiver = [&]() -> capy::io_task<> {
            char buf[8192];
            while (got.size() < size)
            {
                auto [ec, n] = co_await to.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                if (ec)
                    co_return {ec};
                got.append(buf, n);
            }
            co_return {std::error_code{}};
        };
        std::ignore = co_await capy::when_all(sender(), receiver());

        BOOST_TEST_EQ(got.size(), size);
        BOOST_TEST(got == payload);
    };

    bool done          = false;
    bool failsafe_hit  = false;
    std::stop_source failsafe_stop;

    auto run_all = [&]() -> capy::task<> {
        for (std::size_t size : {std::size_t{16384}, std::size_t{16385}})
        {
            co_await transfer(client, server, size, 'c');
            co_await transfer(server, client, size, 's');
        }
        done = true;
        failsafe_stop.request_stop();
    };
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(5000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(run_all());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(done);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** shutdown() while a read is parked on the transport: the reader
    observes end of stream and both shutdowns complete. */
template<typename StreamFactory>
void
testShutdownOverRead(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    bool reader_eof     = false;
    bool local_sd_ok    = false;
    bool peer_sd_ok     = false;
    bool failsafe_hit   = false;
    std::stop_source failsafe_stop;

    // The server parks a read, then shuts down while it is parked. The
    // reader must complete with eof (peer close_notify), not hang.
    auto server_reader = [&]() -> capy::io_task<> {
        char buf[256];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        reader_eof = (ec == capy::cond::eof);
        co_return {ec};
    };
    auto server_shutdown = [&]() -> capy::io_task<> {
        auto [ec] = co_await server.shutdown();
        local_sd_ok = !ec;
        co_return {ec};
    };
    auto server_task = [&]() -> capy::task<> {
        std::ignore =
            co_await capy::when_all(server_reader(), server_shutdown());
        failsafe_stop.request_stop();
    };

    // Client answers the close_notify with its own shutdown.
    auto client_task = [&]() -> capy::task<> {
        char buf[256];
        auto [rec, rn] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        BOOST_TEST(rec == capy::cond::eof);
        auto [ec] = co_await client.shutdown();
        peer_sd_ok = !ec;
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(reader_eof);
    BOOST_TEST(local_sd_ok);
    BOOST_TEST(peer_sd_ok);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Stream wrapper whose transport completions can be held at
    test-controlled gates.

    Pass-through until armed. When armed, a read_some completion is
    withheld until `read_gate_` is set, and a write_some first sets
    `read_gate_` and then waits for `write_gate_` before touching the
    transport. This forces a peer deposit into the window between
    another operation's engine call and its transport read, an
    interleaving no scheduler ordering reaches reliably over a real
    transport.
*/
struct gated_stream
{
    mocket* m_;
    // One event per direction assumes at most one read_some and one
    // write_some in flight at a time; concurrent same-direction calls
    // would race setting/waiting on the same event.
    capy::async_event* read_gate_  = nullptr;
    capy::async_event* write_gate_ = nullptr;

    template<class MutableBufferSequence>
    capy::io_task<std::size_t>
    read_some(MutableBufferSequence buffers)
    {
        auto [ec, n] = co_await m_->read_some(buffers);
        // Withheld regardless of outcome: an errored completion must
        // stay parked just like a successful one, or the gate could
        // not force the interleaving this stream exists to create.
        if (read_gate_)
            std::ignore = co_await read_gate_->wait();
        co_return {ec, n};
    }

    template<class ConstBufferSequence>
    capy::io_task<std::size_t>
    write_some(ConstBufferSequence buffers)
    {
        if (write_gate_)
        {
            if (read_gate_)
                read_gate_->set();
            std::ignore = co_await write_gate_->wait();
        }
        co_return co_await m_->write_some(buffers);
    }
};

/** Simultaneous close over a parked read: the peer's close_notify is
    consumed by the parked reader while shutdown's own close_notify
    flush is still in flight. Shutdown must then retry the engine
    rather than park on a transport read the peer (which legitimately
    keeps the connection open) will never satisfy. */
template<typename StreamFactory>
void
testShutdownSimultaneousClose(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    gated_stream gs{&m1};
    auto client = make_stream(gs, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    bool reader_eof   = false;
    bool local_sd_ok  = false;
    bool peer_sd_ok   = false;
    bool failsafe_hit = false;
    std::stop_source failsafe_stop;

    // Choreography: the client reader parks; the client shutdown
    // stages its close_notify and its flush releases the read gate,
    // then holds. The server's close_notify now completes the parked
    // read, the reader consumes it to eof and releases the write
    // gate, and only then does the shutdown's flush (and whatever
    // follows it) resume — with the deposit already consumed.
    capy::async_event read_gate;
    capy::async_event write_gate;
    gs.read_gate_  = &read_gate;
    gs.write_gate_ = &write_gate;

    auto client_reader = [&]() -> capy::io_task<> {
        char buf[256];
        auto [ec, n] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        reader_eof = (ec == capy::cond::eof);
        write_gate.set();
        // eof is this test's expected outcome; returning it would make
        // when_all stop-request the gated shutdown sibling.
        co_return {};
    };
    auto client_shutdown = [&]() -> capy::io_task<> {
        auto [ec] = co_await client.shutdown();
        local_sd_ok = !ec;
        co_return {ec};
    };
    auto client_task = [&]() -> capy::task<> {
        std::ignore =
            co_await capy::when_all(client_reader(), client_shutdown());
        failsafe_stop.request_stop();
    };

    // The server initiates its own shutdown and then keeps the TCP
    // connection open: a peer that has sent close_notify is not
    // required to close the transport.
    auto server_task = [&]() -> capy::task<> {
        auto [ec] = co_await server.shutdown();
        peer_sd_ok = !ec;
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
            // A gate-wait parks on an in-process event, not transport
            // I/O; closing the mockets alone would leave it stuck, so
            // the failure would hang instead of surfacing.
            read_gate.set();
            write_gate.set();
        }
    };

    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(reader_eof);
    BOOST_TEST(local_sd_ok);
    BOOST_TEST(peer_sd_ok);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Stream wrapper that injects an error alongside a real transport
    read, exercising the ReadStream contract's legal partial-transfer-
    with-error case (IOCP's failed completions carry bytes_transferred;
    a canceled read can likewise deliver bytes with `canceled`).

    read_some delegates to the mocket and, while armed, replaces a
    clean result's error code with the injected one — the byte count
    and any bytes already copied into the caller's buffer are left
    exactly as the delegate produced them. write_some always delegates
    untouched.
*/
struct partial_error_stream
{
    mocket* m_;
    bool armed_ = false;
    std::error_code inject_ec_{};

    template<class MutableBufferSequence>
    capy::io_task<std::size_t>
    read_some(MutableBufferSequence buffers)
    {
        auto [ec, n] = co_await m_->read_some(buffers);
        // Only override a clean result: this stream exists to prove
        // bytes survive an injected error, not to mask a real one.
        if (armed_ && !ec)
            ec = inject_ec_;
        co_return {ec, n};
    }

    template<class ConstBufferSequence>
    capy::io_task<std::size_t>
    write_some(ConstBufferSequence buffers)
    {
        co_return co_await m_->write_some(buffers);
    }
};

/** ReadStream permits `n>0` alongside an error; a transport read that
    delivers both must still leave those bytes visible to the next
    read. This is the failure mode fill_input's `if (ec) co_return ec;`
    used to hit before accounting the delivered bytes: the ciphertext
    handed over with the error would be silently discarded, and the
    next read would park forever waiting for the peer to resend bytes
    it already sent. */
template<typename StreamFactory>
void
testPartialReadWithError(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    // partial_error_stream wraps a mocket specifically (m1's type), so
    // the client (not the server) is the side that gets the injected
    // error; the server plays the plain peer writing plaintext.
    partial_error_stream pes{&m1};
    auto client = make_stream(pes, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    std::string const plaintext(256, 'p');
    bool server_write_ok    = false;
    bool first_read_errored = false;
    bool second_read_ok     = false;
    bool failsafe_hit       = false;
    std::stop_source failsafe_stop;
    capy::async_event write_done;

    auto server_task = [&]() -> capy::task<> {
        auto [ec, n] = co_await capy::write(
            server, capy::const_buffer(plaintext.data(), plaintext.size()));
        server_write_ok = !ec && n == plaintext.size();
        write_done.set();
    };

    // connection_reset is not remapped by the read path's WANT_READ
    // handler (only eof is), so it survives to the caller unchanged
    // and makes a clean probe for "was the error swallowed."
    auto client_task = [&]() -> capy::task<> {
        std::ignore = co_await write_done.wait();
        pes.armed_     = true;
        pes.inject_ec_ = std::make_error_code(std::errc::connection_reset);

        char buf[512];
        auto [ec1, n1] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        (void)n1;
        first_read_errored = (ec1 == std::errc::connection_reset);

        pes.armed_ = false;
        auto [ec2, n2] = co_await client.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        second_read_ok = !ec2 && n2 == plaintext.size() &&
            std::string_view(buf, n2) == plaintext;

        failsafe_stop.request_stop();
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(server_task());
    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(server_write_ok);
    BOOST_TEST(first_read_errored);
    BOOST_TEST(second_read_ok);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Cancelling a parked reader releases its transport claim: a second
    server read issued after the cancellation re-acquires that claim and
    completes on its own — a leaked claim would instead park it forever
    (caught by the failsafe). The writer completing is a separate
    property (read and write hold independent claims); it is not itself
    evidence of the claim release. */
template<typename StreamFactory>
void
testCancelParkedReader(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    {
        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();
    }

    std::string const reply(1024, 'r');
    char const probe[]           = "poke";
    std::size_t const probe_size = sizeof(probe) - 1;
    bool reader_canceled = false;
    bool writer_ok       = false;
    bool client_got_all  = false;
    bool second_read_ok  = false;
    bool failsafe_hit    = false;
    std::stop_source reader_stop;
    std::stop_source failsafe_stop;
    std::stop_source second_read_stop;
    capy::async_event reader_done;

    auto server_reader = [&]() -> capy::task<> {
        char buf[256];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        reader_canceled = (ec == capy::cond::canceled);
        reader_done.set();
    };
    auto server_writer = [&]() -> capy::task<> {
        auto [ec, n] = co_await capy::write(
            server, capy::const_buffer(reply.data(), reply.size()));
        writer_ok = !ec && n == reply.size();
    };
    // Waits for the cancellation to finish (not merely be requested)
    // before reading again, so this doesn't race the first read's own
    // rd_cm_ release; a regression that leaks the claim hangs this read
    // instead of silently passing.
    auto server_second_reader = [&]() -> capy::task<> {
        std::ignore = co_await reader_done.wait();
        char buf[64];
        auto [ec, n] = co_await server.read_some(
            capy::mutable_buffer(buf, sizeof(buf)));
        second_read_ok = !ec && n == probe_size &&
            std::string_view(buf, n) == std::string_view(probe, probe_size);
        failsafe_stop.request_stop();
    };

    auto client_task = [&]() -> capy::task<> {
        std::string got;
        char buf[1024];
        while (got.size() < reply.size())
        {
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            if (ec)
                break;
            got.append(buf, n);
        }
        client_got_all = (got == reply);
        // Reply received; now cancel the parked server reader.
        reader_stop.request_stop();
        auto [ec, n] = co_await capy::write(
            client, capy::const_buffer(probe, probe_size));
        (void)ec;
        (void)n;
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            if (m1.is_open())
                m1.close(); // NOLINT(bugprone-unused-return-value)
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
            reader_done.set(); // unstick a parked wait on this event too
            // A leaked rd_cm_ claim would park the second read forever
            // on its own lock acquisition, past the point closing the
            // mockets can reach; the failsafe must cancel it directly.
            second_read_stop.request_stop();
        }
    };

    capy::run_async(ioc.get_executor(), reader_stop.get_token())(
        server_reader());
    capy::run_async(ioc.get_executor())(server_writer());
    capy::run_async(ioc.get_executor(), second_read_stop.get_token())(
        server_second_reader());
    capy::run_async(ioc.get_executor())(client_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(reader_canceled);
    BOOST_TEST(writer_ok);
    BOOST_TEST(client_got_all);
    BOOST_TEST(second_read_ok);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** The documented multithreaded pattern: all operations on one stream
    run within one strand while the context runs on several threads. */
template<typename StreamFactory>
void
testFullDuplexMtStrand(StreamFactory make_stream)
{
    io_context ioc(2);
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    capy::strand client_strand(ioc.get_executor());
    capy::strand server_strand(ioc.get_executor());

    std::string const request(2048, 'q');
    std::string const reply(2048, 'r');
    bool server_done  = false;
    bool client_done  = false;
    bool failsafe_hit = false;
    std::stop_source failsafe_stop;

    // Session outcomes are stashed here and asserted after both worker
    // threads join, rather than from inside the strand tasks: the
    // counters BOOST_TEST maintains are atomic, but a failing assertion
    // still logs from whichever session thread hit it, interleaved with
    // the other thread's output, and this file's other MT-adjacent tests
    // already defer to the post-join point for that reason.
    std::error_code server_hs_ec;
    std::error_code client_hs_ec;
    std::error_code client_write_ec;
    std::size_t client_got_size = 0;

    auto server_reader = [&]() -> capy::io_task<> {
        char buf[2048];
        for (;;)
        {
            auto [ec, n] = co_await server.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            if (ec)
                co_return {ec};
        }
    };
    auto server_writer = [&]() -> capy::io_task<> {
        auto [ec, n] = co_await capy::write(
            server, capy::const_buffer(reply.data(), reply.size()));
        co_return {ec};
    };
    auto server_task = [&]() -> capy::task<> {
        auto [ec]    = co_await server.handshake(tls_role::server);
        server_hs_ec = ec;
        if (ec)
        {
            // A red handshake means the rest of this test can never
            // complete; don't make it also pay the full failsafe wait.
            failsafe_stop.request_stop();
            co_return;
        }
        std::ignore =
            co_await capy::when_all(server_reader(), server_writer());
        server_done = true;
        failsafe_stop.request_stop();
    };

    auto client_task = [&]() -> capy::task<> {
        auto [hec]   = co_await client.handshake(tls_role::client);
        client_hs_ec = hec;
        if (hec)
        {
            failsafe_stop.request_stop();
            co_return;
        }
        auto [wec, wn] = co_await capy::write(
            client, capy::const_buffer(request.data(), request.size()));
        client_write_ec = wec;

        std::string got;
        char buf[2048];
        while (got.size() < reply.size())
        {
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            if (ec)
                break;
            got.append(buf, n);
        }
        client_got_size = got.size();
        client_done     = true;
        m1.close(); // NOLINT(bugprone-unused-return-value)
    };

    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(5000 * failsafe_scale));
        if (!ec)
        {
            failsafe_hit = true;
            // Close through each owning strand: the mockets aren't
            // safe for concurrent access, and only the strand's
            // serialization (not this task's thread) makes touching
            // them from the session tasks non-concurrent.
            auto close_m1 = [&]() -> capy::task<> {
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                co_return;
            };
            auto close_m2 = [&]() -> capy::task<> {
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
                co_return;
            };
            capy::run_async(client_strand)(close_m1());
            capy::run_async(server_strand)(close_m2());
        }
    };

    capy::run_async(server_strand)(server_task());
    capy::run_async(client_strand)(client_task());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());

    std::thread t([&ioc] { ioc.run(); });
    ioc.run();
    t.join();

    BOOST_TEST(!server_hs_ec);
    BOOST_TEST(!client_hs_ec);
    BOOST_TEST(!client_write_ec);
    BOOST_TEST_EQ(client_got_size, reply.size());

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(client_done);
    BOOST_TEST(server_done);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

/** Test that a trailing flush failure is deferred to the next operation.

    A write whose payload the engine fully accepts still issues a
    trailing transport flush; if that flush fails, the stream contract
    requires `n == size` with no error (a partial-looking result would
    make `capy::write`'s composed loop drop the error and report
    success on a lost final record). This confirms the error is
    stashed instead and surfaces on the very next write, and on
    shutdown().
*/
template<typename StreamFactory>
void
testDeferredFlushError(StreamFactory make_stream)
{
    char const data[] = "test";
    std::size_t const size = sizeof(data) - 1;

    // Write side: the flush error is deferred off a full transfer,
    // then surfaces on the next write with nothing transferred.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        // The client's own transport dies after the handshake; the
        // engine still accepts the next write into its output buffer
        // before the trailing flush discovers the dead descriptor.
        m1.socket().close();

        std::error_code ec1;
        std::size_t n1 = 0;
        auto write1 = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(data, size));
            ec1 = ec;
            n1  = n;
        };
        capy::run_async(ioc.get_executor())(write1());
        ioc.run();
        ioc.restart();

        BOOST_TEST(!ec1);
        BOOST_TEST_EQ(n1, size);

        std::error_code ec2;
        std::size_t n2      = 1;
        bool failsafe_hit   = false;
        std::stop_source failsafe_stop;
        auto write2 = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(data, size));
            ec2 = ec;
            n2  = n;
            failsafe_stop.request_stop();
        };
        auto failsafe_task = [&]() -> capy::task<> {
            auto [ec] = co_await corosio::delay(
                std::chrono::milliseconds(2000 * failsafe_scale));
            if (!ec)
            {
                failsafe_hit = true;
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
            }
        };
        capy::run_async(ioc.get_executor())(write2());
        capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
            failsafe_task());
        ioc.run();

        BOOST_TEST(!failsafe_hit);
        BOOST_TEST(!!ec2);
        BOOST_TEST_EQ(n2, 0u);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }

    // Read side: the same stash surfaces when the next operation after
    // the deferred write is a read instead of a write, and the read
    // right after that (stash now empty) goes through the ordinary path
    // instead of replaying it.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        m1.socket().close();

        std::error_code ec1;
        std::size_t n1 = 0;
        auto write1 = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(data, size));
            ec1 = ec;
            n1  = n;
        };
        capy::run_async(ioc.get_executor())(write1());
        ioc.run();
        ioc.restart();

        // As above: nothing is deferred unless this write reported a
        // full, clean transfer.
        BOOST_TEST(!ec1);
        BOOST_TEST_EQ(n1, size);

        std::error_code ec2;
        std::size_t n2      = 1;
        bool failsafe_hit   = false;
        std::stop_source failsafe_stop;
        auto read1 = [&]() -> capy::task<> {
            char buf[64];
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            ec2 = ec;
            n2  = n;
            failsafe_stop.request_stop();
        };
        auto failsafe_task = [&]() -> capy::task<> {
            auto [ec] = co_await corosio::delay(
                std::chrono::milliseconds(2000 * failsafe_scale));
            if (!ec)
            {
                failsafe_hit = true;
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
            }
        };
        capy::run_async(ioc.get_executor())(read1());
        capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
            failsafe_task());
        ioc.run();
        ioc.restart();

        // The read surfaces the write's stashed flush error, not its own
        // (a read never flushes anything on this path): n == 0 either way,
        // so nothing was silently transferred alongside the error.
        BOOST_TEST(!failsafe_hit);
        BOOST_TEST(!!ec2);
        BOOST_TEST_EQ(n2, 0u);

        std::error_code ec3;
        std::size_t n3       = 1;
        bool failsafe_hit2   = false;
        std::stop_source failsafe_stop2;
        auto read2 = [&]() -> capy::task<> {
            char buf[64];
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            ec3 = ec;
            n3  = n;
            failsafe_stop2.request_stop();
        };
        auto failsafe_task2 = [&]() -> capy::task<> {
            auto [ec] = co_await corosio::delay(
                std::chrono::milliseconds(2000 * failsafe_scale));
            if (!ec)
            {
                failsafe_hit2 = true;
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
            }
        };
        capy::run_async(ioc.get_executor())(read2());
        capy::run_async(ioc.get_executor(), failsafe_stop2.get_token())(
            failsafe_task2());
        ioc.run();

        // The stash is consumed exactly once: this read takes the
        // ordinary path (the dead transport, discovered fresh, is its
        // own independent failure) rather than hanging or replaying the
        // first read's error.
        BOOST_TEST(!failsafe_hit2);
        BOOST_TEST(!!ec3);
        BOOST_TEST_EQ(n3, 0u);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }

    // shutdown() surfaces a stash left by an earlier deferred flush.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        m1.socket().close();

        std::error_code ec1;
        std::size_t n1 = 0;
        auto write1 = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(data, size));
            ec1 = ec;
            n1  = n;
        };
        capy::run_async(ioc.get_executor())(write1());
        ioc.run();
        ioc.restart();

        // The stash shutdown() is expected to surface only exists if
        // this write reported a full, clean transfer (a partial or
        // errored write here would mean there's nothing deferred, and
        // the assertions below would be proving nothing).
        BOOST_TEST(!ec1);
        BOOST_TEST_EQ(n1, size);

        std::error_code sec;
        bool failsafe_hit = false;
        std::stop_source failsafe_stop;
        auto shutdown_task = [&]() -> capy::task<> {
            auto [ec] = co_await client.shutdown();
            sec = ec;
            failsafe_stop.request_stop();
        };
        auto failsafe_task = [&]() -> capy::task<> {
            auto [ec] = co_await corosio::delay(
                std::chrono::milliseconds(2000 * failsafe_scale));
            if (!ec)
            {
                failsafe_hit = true;
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
            }
        };
        capy::run_async(ioc.get_executor())(shutdown_task());
        capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
            failsafe_task());
        ioc.run();

        BOOST_TEST(!failsafe_hit);
        BOOST_TEST(!!sec);
        // The stash is known to hold *some* error (asserted above); it
        // cannot be distinguished here from an independent failure
        // shutdown() hits on its own, since both share the same dead
        // transport. stream_truncated is shutdown()'s own normalized
        // outcome for a transport that vanishes without close_notify, so
        // ruling it out is the closest available proxy for "this is the
        // stashed write error, not shutdown()'s own" without adding a
        // test-only accessor into pending_flush_ec_.
        BOOST_TEST(sec != capy::cond::stream_truncated);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Edge behavior at the end of a stream's life: a second shutdown(),
    read_some/write_some issued after a completed shutdown, and
    zero-length buffer sequences.

    (a) is pinned identically on both backends: a second shutdown() is
    idempotent success. The post-shutdown read/write in (b) diverge by
    backend and are asserted per the discovered shape (noted inline):
    OpenSSL caches the received-shutdown state so a read completes with
    eof immediately, and maps a write after our own close_notify to a
    protocol error; WolfSSL does not cache that state across a
    shutdown-driven completion (only a read that itself observes the
    close_notify latches it), so a read issued afterward genuinely waits
    on the transport, and WolfSSL permits the write outright (no local
    half-close enforcement). (c) is pinned identically on both backends:
    a zero-length buffer is a clean `{ {}, 0 }` completion, matching the
    plain-socket contract.
*/
template<typename StreamFactory>
void
testTlsLifecycleEdges(StreamFactory make_stream)
{
    // (a) shutdown() called twice.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        std::error_code c_sd1, s_sd1;
        auto client_sd = [&]() -> capy::task<> {
            auto [ec] = co_await client.shutdown();
            c_sd1 = ec;
        };
        auto server_sd = [&]() -> capy::task<> {
            auto [ec] = co_await server.shutdown();
            s_sd1 = ec;
        };
        capy::run_async(ioc.get_executor())(client_sd());
        capy::run_async(ioc.get_executor())(server_sd());
        ioc.run();
        ioc.restart();
        BOOST_TEST(!c_sd1);
        BOOST_TEST(!s_sd1);

        std::error_code c_sd2;
        auto client_sd2 = [&]() -> capy::task<> {
            auto [ec] = co_await client.shutdown();
            c_sd2 = ec;
        };
        capy::run_async(ioc.get_executor())(client_sd2());
        ioc.run();
        // Idempotent success on both backends: the same deterministic
        // outcome regardless of which engine is underneath.
        BOOST_TEST(!c_sd2);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }

    // (b) read_some after a completed shutdown.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        auto client_sd = [&]() -> capy::task<> {
            (void)co_await client.shutdown();
        };
        auto server_sd = [&]() -> capy::task<> {
            (void)co_await server.shutdown();
        };
        capy::run_async(ioc.get_executor())(client_sd());
        capy::run_async(ioc.get_executor())(server_sd());
        ioc.run();
        ioc.restart();

        // A read after both close_notifies have been exchanged reports
        // end of stream on either backend: the peer's shutdown is
        // latched, so the read completes at once rather than waiting on
        // the transport for bytes that will never come.
        std::error_code rd_ec;
        std::size_t rd_n = 1;
        auto read_op    = [&]() -> capy::task<> {
            char buf[64];
            auto [ec, n] = co_await client.read_some(
                capy::mutable_buffer(buf, sizeof(buf)));
            rd_ec = ec;
            rd_n  = n;
        };
        capy::run_async(ioc.get_executor())(read_op());
        ioc.run();

        BOOST_TEST(rd_ec == capy::cond::eof);
        BOOST_TEST_EQ(rd_n, 0u);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }

    // write_some after a completed shutdown, on its own transport so its
    // outcome cannot be contaminated by the read above.
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        auto client_sd = [&]() -> capy::task<> {
            (void)co_await client.shutdown();
        };
        auto server_sd = [&]() -> capy::task<> {
            (void)co_await server.shutdown();
        };
        capy::run_async(ioc.get_executor())(client_sd());
        capy::run_async(ioc.get_executor())(server_sd());
        ioc.run();
        ioc.restart();

        char one = 'z';
        std::error_code wr_ec;
        std::size_t wr_n   = 0;
        bool wfailsafe_hit = false;
        std::stop_source wfailsafe_stop;
        auto write_op = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(&one, 1));
            wr_ec = ec;
            wr_n  = n;
            wfailsafe_stop.request_stop();
        };
        auto wfailsafe_task = [&]() -> capy::task<> {
            auto [ec] = co_await corosio::delay(
                std::chrono::milliseconds(2000 * failsafe_scale));
            if (!ec)
            {
                wfailsafe_hit = true;
                if (m1.is_open())
                    m1.close(); // NOLINT(bugprone-unused-return-value)
                if (m2.is_open())
                    m2.close(); // NOLINT(bugprone-unused-return-value)
            }
        };
        capy::run_async(ioc.get_executor())(write_op());
        capy::run_async(ioc.get_executor(), wfailsafe_stop.get_token())(
            wfailsafe_task());
        ioc.run();

        BOOST_TEST(!wfailsafe_hit);
        if (wr_n == 1)
            // WolfSSL: permits an application write after our own
            // close_notify (no local half-close enforcement); the byte
            // is queued and reported delivered.
            BOOST_TEST(!wr_ec);
        else
        {
            // OpenSSL: maps a write after our own close_notify to a
            // protocol error.
            BOOST_TEST(!!wr_ec);
            BOOST_TEST_EQ(wr_n, 0u);
        }

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }

    // (c) zero-length buffer sequences: clean { {}, 0 } completion,
    // matching the plain-socket contract (no transport I/O needed).
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = make_client_context();
        auto server_ctx = make_server_context();

        auto client = make_stream(m1, client_ctx);
        auto server = make_stream(m2, server_ctx);

        auto hs_client = [&]() -> capy::task<> {
            auto [ec] = co_await client.handshake(tls_role::client);
            BOOST_TEST(!ec);
        };
        auto hs_server = [&]() -> capy::task<> {
            auto [ec] = co_await server.handshake(tls_role::server);
            BOOST_TEST(!ec);
        };
        capy::run_async(ioc.get_executor())(hs_client());
        capy::run_async(ioc.get_executor())(hs_server());
        ioc.run();
        ioc.restart();

        std::error_code wec, rec;
        std::size_t wn = 1, rn = 1;
        auto zero_ops = [&]() -> capy::task<> {
            auto [ec1, n1] = co_await client.write_some(
                capy::const_buffer(nullptr, 0));
            wec = ec1;
            wn  = n1;
            auto [ec2, n2] = co_await client.read_some(
                capy::mutable_buffer(nullptr, 0));
            rec = ec2;
            rn  = n2;
        };
        capy::run_async(ioc.get_executor())(zero_ops());
        ioc.run();

        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 0u);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 0u);

        if (m1.is_open())
            m1.close(); // NOLINT(bugprone-unused-return-value)
        if (m2.is_open())
            m2.close(); // NOLINT(bugprone-unused-return-value)
    }
}

/** Test that shutdown() reports truncation when the peer vanishes
    without a close_notify.

    An unannounced transport close during shutdown is indistinguishable
    from an attacker truncating the stream, so it must surface as
    `stream_truncated`, not success.
*/
template<typename StreamFactory>
void
testShutdownTruncation(StreamFactory make_stream)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client_ctx = make_client_context();
    auto server_ctx = make_server_context();

    auto client = make_stream(m1, client_ctx);
    auto server = make_stream(m2, server_ctx);

    auto hs_client = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        BOOST_TEST(!ec);
    };
    auto hs_server = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        BOOST_TEST(!ec);
    };
    capy::run_async(ioc.get_executor())(hs_client());
    capy::run_async(ioc.get_executor())(hs_server());
    ioc.run();
    ioc.restart();

    // Client's transport sends a FIN without a TLS close_notify. A
    // half-close (not a full close) avoids discarding any unread
    // inbound bytes (e.g. a post-handshake session ticket), which
    // would otherwise turn the FIN into an RST and hit a different
    // (already-propagated) error path than the one under test.
    BOOST_TEST(!m1.socket().shutdown(shutdown_send));

    bool shutdown_done = false;
    bool failsafe_hit  = false;
    std::error_code shutdown_ec;
    std::stop_source failsafe_stop;

    auto server_shutdown = [&]() -> capy::task<> {
        auto [ec]     = co_await server.shutdown();
        shutdown_ec   = ec;
        shutdown_done = true;
        failsafe_stop.request_stop();
    };

    // Failsafe deadline in case of bugs, not a timing assertion: the
    // pass/fail signal below is shutdown_ec, not how long it took.
    auto failsafe_task = [&]() -> capy::task<> {
        auto [ec] = co_await corosio::delay(
            std::chrono::milliseconds(2000 * failsafe_scale));
        if (!ec && !shutdown_done)
        {
            failsafe_hit = true;
            if (m2.is_open())
                m2.close(); // NOLINT(bugprone-unused-return-value)
        }
    };

    capy::run_async(ioc.get_executor())(server_shutdown());
    capy::run_async(ioc.get_executor(), failsafe_stop.get_token())(
        failsafe_task());
    ioc.run();

    BOOST_TEST(!failsafe_hit);
    BOOST_TEST(shutdown_done);
    BOOST_TEST(shutdown_ec == capy::cond::stream_truncated);

    if (m1.is_open())
        m1.close(); // NOLINT(bugprone-unused-return-value)
    if (m2.is_open())
        m2.close(); // NOLINT(bugprone-unused-return-value)
}

} // namespace boost::corosio::test

#endif
