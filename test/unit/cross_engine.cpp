//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Cross-backend engine interop: an OpenSSL engine and a WolfSSL
// engine complete handshakes and exchange data through the same
// in-memory byte shuttle, transport-free.

#include <boost/corosio/tls_context.hpp>

#if defined(BOOST_COROSIO_HAS_OPENSSL) && defined(BOOST_COROSIO_HAS_WOLFSSL)

// The engine headers are vendor-free precisely so this TU can
// exist: the real OpenSSL and WolfSSL headers cannot coexist in one
// TU (WolfSSL's OpenSSL-compat layer clashes with genuine OpenSSL
// declarations on some builds).
#include "src/openssl/src/detail/engine.hpp"
#include "src/wolfssl/src/detail/engine.hpp"

#ifdef OPENSSL_VERSION_NUMBER
#error cross_engine.cpp must stay vendor-header-free
#endif
#ifdef WOLFSSL_VERSION
#error cross_engine.cpp must stay vendor-header-free
#endif

#include "engine_shuttle.hpp"
#include "test_utils.hpp"
#include "test_suite.hpp"

#include <boost/capy/error.hpp>

#include <cstddef>
#include <string>
#include <system_error>

namespace boost::corosio {

namespace {

using detail::engine_op;

// The two engines spell `init` differently: WolfSSL binds the role at
// session creation (its client/server methods differ), while OpenSSL
// derives it from the first handshake op. These adapters let one
// templated scenario drive either backend.
inline std::error_code
parity_init(detail::openssl::engine& e, tls_context const& c, tls_role)
{
    return e.init(c);
}

inline std::error_code
parity_init(
    detail::wolfssl::engine& e, tls_context const& c, tls_role role)
{
    return e.init(c, role, std::string());
}

// Observable outcomes of the parity scenario, captured per backend so
// the two can be compared for equality.
struct parity_result
{
    bool handshake_ok            = false;
    bool wrong_ca_rejected       = false;
    bool shutdown_ok             = false;
    bool both_received_shutdown  = false;
    std::error_code read_after_close_ec;
    std::size_t read_after_close_n = ~std::size_t{0};
};

// Run the parity scenario through a same-backend engine pair.
template<class Engine>
parity_result
run_parity()
{
    parity_result out;

    // A handshake against a trusted CA succeeds, followed by a clean
    // bidirectional shutdown and a read past the close.
    {
        auto cctx = test::make_client_context();
        auto sctx = test::make_server_context();
        Engine client;
        Engine server;
        BOOST_TEST(!parity_init(client, cctx, tls_role::client));
        BOOST_TEST(!parity_init(server, sctx, tls_role::server));

        out.handshake_ok = test::run_engine_handshake(client, server);
        if (out.handshake_ok)
        {
            out.shutdown_ok = test::run_engine_shutdown(client, server);
            out.both_received_shutdown =
                client.received_shutdown() && server.received_shutdown();

            char buf[64];
            auto const r = client.perform(engine_op::read, buf, sizeof(buf));
            out.read_after_close_ec = r.ec;
            out.read_after_close_n  = r.bytes;
        }
    }

    // A server certificate the client does not trust is rejected, never
    // silently accepted by one backend.
    {
        auto cctx = test::make_wrong_ca_context();
        auto sctx = test::make_server_context();
        Engine client;
        Engine server;
        // The wrong CA is a valid, loadable trust anchor that simply does
        // not sign the server, so init succeeds on both backends and the
        // rejection lands at the handshake (no signer to confirm the
        // server's certificate).
        BOOST_TEST(!parity_init(client, cctx, tls_role::client));
        BOOST_TEST(!parity_init(server, sctx, tls_role::server));
        out.wrong_ca_rejected =
            !test::run_engine_handshake(client, server);
    }

    return out;
}

} // namespace

struct cross_engine_test
{
    template<class Client, class Server>
    void
    exchange(Client& client, Server& server)
    {
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::string const c2s = "engine interop client to server";
        std::string const s2c = "engine interop server to client";
        std::string got;

        BOOST_TEST(test::engine_send(client, server, c2s));
        BOOST_TEST(test::engine_recv(server, client, got, c2s.size()));
        BOOST_TEST(got == c2s);

        BOOST_TEST(test::engine_send(server, client, s2c));
        BOOST_TEST(test::engine_recv(client, server, got, s2c.size()));
        BOOST_TEST(got == s2c);
    }

    void
    testOpensslClientWolfsslServer()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        detail::openssl::engine client;
        detail::wolfssl::engine server;
        BOOST_TEST(!client.init(client_ctx));
        BOOST_TEST(
            !server.init(server_ctx, tls_role::server, std::string()));
        exchange(client, server);
    }

    void
    testWolfsslClientOpensslServer()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        detail::wolfssl::engine client;
        detail::openssl::engine server;
        BOOST_TEST(
            !client.init(client_ctx, tls_role::client, std::string()));
        BOOST_TEST(!server.init(server_ctx));
        exchange(client, server);
    }

    /** Every meaningful `map_fill_error` cell, direct.

        A pure function, cheap to exhaust exactly rather than sample
        through the engines that happen to call it.
    */
    void
    testMapFillErrorTable()
    {
        using detail::engine_op;
        using detail::map_fill_error;

        // shutdown: an empty ec and canceled pass through untouched,
        // ahead of the close-family / received-shutdown logic below.
        BOOST_TEST(!map_fill_error(engine_op::shutdown, {}, false));
        BOOST_TEST(
            map_fill_error(
                engine_op::shutdown, make_error_code(capy::error::canceled),
                true) == capy::error::canceled);

        // shutdown: every close-family member, both received states.
        std::error_code const family[] = {
            make_error_code(capy::error::eof),
            make_error_code(std::errc::connection_reset),
            make_error_code(std::errc::connection_aborted),
            make_error_code(std::errc::broken_pipe)};
        for (auto const& ec : family)
        {
            BOOST_TEST(!map_fill_error(engine_op::shutdown, ec, true));
            BOOST_TEST(
                map_fill_error(engine_op::shutdown, ec, false) ==
                capy::error::stream_truncated);
        }

        // shutdown: outside the family, the received bit must never
        // swallow an unrelated fault.
        BOOST_TEST(
            map_fill_error(
                engine_op::shutdown,
                make_error_code(std::errc::invalid_argument), true) ==
            std::errc::invalid_argument);

        // read / write: eof remaps on the received bit; anything else
        // passes through.
        for (auto op : {engine_op::read, engine_op::write})
        {
            BOOST_TEST(
                map_fill_error(op, make_error_code(capy::error::eof), true) ==
                capy::error::eof);
            BOOST_TEST(
                map_fill_error(op, make_error_code(capy::error::eof), false) ==
                capy::error::stream_truncated);
            BOOST_TEST(
                map_fill_error(
                    op, make_error_code(std::errc::connection_reset), true) ==
                std::errc::connection_reset);
        }

        // handshake: every error passes through unchanged, including the
        // exact eof code read/write would otherwise remap.
        for (auto op :
             {engine_op::handshake_client, engine_op::handshake_server})
            for (bool received : {false, true})
                BOOST_TEST(
                    map_fill_error(
                        op, make_error_code(capy::error::eof), received) ==
                    capy::error::eof);
    }

    /** The two backends behave identically on the operations that must
        not diverge.

        The shared stream suite runs the same scenarios against each
        backend but tolerates legitimate library differences via runtime
        branches (a write after our own close_notify: WolfSSL queues it,
        OpenSSL faults). This pins the behaviors that must be the same,
        asserting the WolfSSL and OpenSSL outcomes are equal rather than
        merely individually valid.

        Out of scope by design: write-after-close (documented divergence)
        and raw library error codes (necessarily backend-specific).
        Transport-truncation mapping is covered exhaustively by
        `testMapFillErrorTable`.
    */
    void
    testBackendParity()
    {
        auto const w = run_parity<detail::wolfssl::engine>();
        auto const o = run_parity<detail::openssl::engine>();

        // Handshake success.
        BOOST_TEST(w.handshake_ok == o.handshake_ok);
        BOOST_TEST(w.handshake_ok);

        // Handshake failure: an untrusted server cert is rejected by both.
        BOOST_TEST(w.wrong_ca_rejected == o.wrong_ca_rejected);
        BOOST_TEST(w.wrong_ca_rejected);

        // Clean bidirectional shutdown completes and latches the peer's
        // close on both.
        BOOST_TEST(w.shutdown_ok == o.shutdown_ok);
        BOOST_TEST(w.shutdown_ok);
        BOOST_TEST(w.both_received_shutdown == o.both_received_shutdown);
        BOOST_TEST(w.both_received_shutdown);

        // A read after the close reports eof with no bytes on both, rather
        // than parking or surfacing a raw library code. This is the parity
        // the minimal-WolfSSL close-state handling must preserve.
        BOOST_TEST(w.read_after_close_ec == o.read_after_close_ec);
        BOOST_TEST(w.read_after_close_ec == capy::error::eof);
        BOOST_TEST_EQ(w.read_after_close_n, o.read_after_close_n);
        BOOST_TEST_EQ(w.read_after_close_n, std::size_t{0});
    }

    void
    run()
    {
        testOpensslClientWolfsslServer();
        testWolfsslClientOpensslServer();
        testMapFillErrorTable();
        testBackendParity();
    }
};

TEST_SUITE(cross_engine_test, "boost.corosio.cross_engine");

} // namespace boost::corosio

#endif
