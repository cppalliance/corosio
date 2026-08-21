//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Transport-free unit tests for the WolfSSL record engine: two
// engines shuttle bytes in memory, so every want/ec mapping in
// `perform` is observable directly instead of through a stream.

#include <boost/corosio/tls_context.hpp>

#ifdef BOOST_COROSIO_HAS_WOLFSSL

// wolfssl_category lives with the public stream class
#include <boost/corosio/wolfssl_stream.hpp>

#include "src/wolfssl/src/detail/engine.hpp"

// The engine header is vendor-free; this single-vendor TU pulls in
// the real headers itself for the native-handle tests (key update).
// WolfSSL options must precede every other WolfSSL header so feature
// macros match the linked library.
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "engine_shuttle.hpp"
#include "test_utils.hpp"
#include "test_suite.hpp"

#include <boost/capy/error.hpp>

#include <string>
#include <vector>
#include <tuple>

namespace boost::corosio {

namespace {

using detail::engine_op;
using detail::engine_want;
using wssl_engine = detail::wolfssl::engine;

// Init both engines from cert contexts; contexts are caller-owned
// because the engines reference the cached native contexts they carry.
bool
init_pair(
    wssl_engine& client,
    wssl_engine& server,
    tls_context const& client_ctx,
    tls_context const& server_ctx)
{
    return !client.init(client_ctx, tls_role::client, std::string()) &&
        !server.init(server_ctx, tls_role::server, std::string());
}

} // namespace

struct wolfssl_engine_test
{
    void
    testHandshake()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));

        // A fresh server with nothing staged must park on input.
        auto rs = server.perform(engine_op::handshake_server, nullptr, 0);
        BOOST_TEST(!rs.ec);
        BOOST_TEST(rs.want == engine_want::input);

        // The client's first step stages the ClientHello: the want
        // protocol demands the flush before parking on input.
        auto rc = client.perform(engine_op::handshake_client, nullptr, 0);
        BOOST_TEST(!rc.ec);
        BOOST_TEST(rc.want == engine_want::output_then_retry);
        BOOST_TEST(client.pending_output() > 0);

        BOOST_TEST(test::run_engine_handshake(client, server));
        BOOST_TEST(!client.received_shutdown());
        BOOST_TEST(!server.received_shutdown());
    }

    void
    testAppDataBothDirections()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::string const c2s = "hello from the client engine";
        std::string const s2c = "hello from the server engine";
        std::string got;

        BOOST_TEST(test::engine_send(client, server, c2s));
        BOOST_TEST(test::engine_recv(server, client, got, c2s.size()));
        BOOST_TEST(got == c2s);

        BOOST_TEST(test::engine_send(server, client, s2c));
        BOOST_TEST(test::engine_recv(client, server, got, s2c.size()));
        BOOST_TEST(got == s2c);
    }

    void
    testCloseNotifySequence()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        // Shutdown queues our close_notify and waits for the peer's:
        // flush first.
        auto r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(server.pending_output() > 0);
        std::vector<unsigned char> wire(server.pending_output());
        std::size_t const n = server.get_output(wire.data(), wire.size());
        BOOST_TEST_EQ(n, wire.size());

        // Re-queried with empty staging and no fresh input, the
        // shutdown parks on input: WolfSSL reports this state as
        // WANT_READ and the mapping must route it to the same
        // flush-then-fill treatment as SHUTDOWN_NOT_DONE.
        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::input);
        BOOST_TEST_EQ(server.pending_output(), 0u);

        // The peer's read observes the close_notify as a clean eof
        // with a plain done: a received close queues no output.
        BOOST_TEST_EQ(client.put_input(wire.data(), n), n);
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(r.want == engine_want::done);
        BOOST_TEST(r.ec == capy::error::eof);
        BOOST_TEST_EQ(r.bytes, 0u);
        BOOST_TEST(client.received_shutdown());

        // Responding shutdown succeeds immediately (both directions
        // closed) but leaves our close_notify staged: the falsy
        // terminal must still route through the reporting flush.
        r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST(client.received_shutdown());
        BOOST_TEST(client.pending_output() > 0);

        // Write-side close observation is not constructible: WolfSSL
        // permits writes even after a completed local shutdown, so
        // only document the read/shutdown observations here.

        // Deliver the response; the initiator's shutdown completes
        // with success and the received-shutdown bit set.
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);
        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(test::engine_step_done(r.want));
        BOOST_TEST(server.received_shutdown());
    }

    void
    testShutdownZeroReturnWithPendingOutput()
    {
        // A close_notify consumed by a parked reader surfaces the
        // shutdown re-query as a zero-return fatal, which is a
        // completed close, not an error. With our own close_notify
        // still staged the verdict must be output_then_done so the
        // driver's reporting flush runs.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        // Both sides initiate; the client's close_notify stays staged.
        auto r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(client.pending_output() > 0);
        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(test::shuttle_bytes(server, client) > 0);

        // The "parked reader" consumes the peer's close_notify.
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(r.ec == capy::error::eof);
        BOOST_TEST(client.received_shutdown());

        // The shutdown re-query: zero-return fatal, success verdict,
        // staged output still to flush.
        r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST(client.pending_output() > 0);

        // No byte loss: the staged close_notify still completes the
        // peer's shutdown.
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);
        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(test::engine_step_done(r.want));
        BOOST_TEST(server.received_shutdown());
    }

    void
    testShutdownBeforeInit()
    {
        // The reachable truthy shutdown terminal: the driver runs
        // shutdown-before-handshake against an engine whose session
        // was never created, and the mapped BAD_FUNC_ARG must come
        // back as a wolfssl-category error.
        wssl_engine eng;
        auto const r = eng.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::done);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == wolfssl_category());
        BOOST_TEST_EQ(eng.pending_output(), 0u);
    }

    void
    testRekeyUpdateKeys()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        // TLS 1.3 rekey: wolfSSL_update_keys stages the KeyUpdate
        // through the engine's send path immediately.
        BOOST_TEST_EQ(wolfSSL_update_keys(server.native_handle()), 1);
        BOOST_TEST(server.pending_output() > 0);
        std::string const msg = "post-rekey payload";
        auto r = server.perform(
            engine_op::write, const_cast<char*>(msg.data()), msg.size());
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST_EQ(r.bytes, msg.size());
        BOOST_TEST(test::shuttle_bytes(server, client) > 0);

        // A staged, unprocessed KeyUpdate is transparent to the
        // peer's write: rekeying gates reads, not writes.
        char one = 'x';
        r = client.perform(engine_op::write, &one, 1);
        BOOST_TEST(!r.ec);
        BOOST_TEST(test::engine_step_done(r.want));
        BOOST_TEST_EQ(r.bytes, 1u);

        // The peer's read consumes the KeyUpdate and stages its own
        // KeyUpdate response: output-first demands the flush ride
        // with the data.
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST_EQ(r.bytes, msg.size());
        BOOST_TEST(std::string(buf, r.bytes) == msg);
        BOOST_TEST(client.pending_output() > 0);
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);

        // The response (and the earlier write) decrypt under the new
        // keys; traffic continues in both directions.
        std::string got;
        BOOST_TEST(test::engine_recv(server, client, got, 1));
        BOOST_TEST(got == "x");
        BOOST_TEST(test::engine_send(server, client, "after"));
        BOOST_TEST(test::engine_recv(client, server, got, 5));
        BOOST_TEST(got == "after");
    }

    void
    testTruncatedRecordWaitsForInput()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::string const msg = "cut mid-record";
        auto r = server.perform(
            engine_op::write, const_cast<char*>(msg.data()), msg.size());
        BOOST_TEST(test::engine_step_done(r.want));
        std::vector<unsigned char> wire(server.pending_output());
        BOOST_TEST_EQ(server.get_output(wire.data(), wire.size()), wire.size());

        // Deliver all but the record tail: the read must park on
        // input, not fail. The truncation error itself is transport
        // policy (`map_fill_error`), decided when eof arrives
        // without a close_notify.
        std::size_t const cut = wire.size() - 5;
        BOOST_TEST_EQ(client.put_input(wire.data(), cut), cut);
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::input);
        BOOST_TEST(!client.received_shutdown());
        BOOST_TEST(
            detail::map_fill_error(
                engine_op::read, make_error_code(capy::error::eof),
                client.received_shutdown()) ==
            capy::error::stream_truncated);

        // The tail completes the record with no byte loss.
        BOOST_TEST_EQ(client.put_input(wire.data() + cut, 5), 5u);
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(!r.ec);
        BOOST_TEST(test::engine_step_done(r.want));
        BOOST_TEST(std::string(buf, r.bytes) == msg);
    }

    void
    testCorruptRecordMapsError()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::string const msg = "tamper target";
        auto r = server.perform(
            engine_op::write, const_cast<char*>(msg.data()), msg.size());
        BOOST_TEST(test::engine_step_done(r.want));
        std::vector<unsigned char> wire(server.pending_output());
        BOOST_TEST_EQ(server.get_output(wire.data(), wire.size()), wire.size());

        wire.back() ^= 0x5a;
        BOOST_TEST_EQ(client.put_input(wire.data(), wire.size()), wire.size());
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        // The failed decrypt queues a fatal alert, so the truthy
        // terminal carries pending output for the best-effort flush.
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST(client.pending_output() > 0);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == wolfssl_category());
        BOOST_TEST_EQ(r.bytes, 0u);
    }

    void
    testPartialPutInputNoLoss()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        // Two max-size records exceed the input staging capacity.
        std::string msg(std::size_t{2} * 16384, '\0');
        for (std::size_t i = 0; i < msg.size(); ++i)
            msg[i] = static_cast<char>('a' + i % 23);

        std::vector<unsigned char> wire;
        // Heap-backed drain chunk: ASan's pointer-pair checker cannot
        // prove same-object for a stack array's one-past-end pointer,
        // which the range insert below computes a distance from.
        // Reserving the ciphertext upper bound avoids reallocation,
        // whose element move can also trip the checker via
        // optimizer-introduced cross-buffer alias checks.
        wire.reserve(msg.size() + 1024);
        std::vector<unsigned char> chunk(8192);
        std::size_t total = 0;
        for (int i = 0; i < 64 && total < msg.size(); ++i)
        {
            auto r = server.perform(
                engine_op::write, msg.data() + total, msg.size() - total);
            BOOST_TEST(!r.ec);
            for (std::size_t n;
                 (n = server.get_output(chunk.data(), chunk.size())) > 0;)
                wire.insert(wire.end(), chunk.data(), chunk.data() + n);
            if (test::engine_step_done(r.want))
                total += r.bytes;
        }
        BOOST_TEST_EQ(total, msg.size());

        // A single oversized offer fills the staging: the engine
        // accepts what fits and the caller keeps the tail.
        std::size_t fed = client.put_input(wire.data(), wire.size());
        BOOST_TEST(fed > 0);
        BOOST_TEST(fed < wire.size());

        std::string got;
        char buf[8192];
        for (int i = 0; i < 64 && got.size() < msg.size(); ++i)
        {
            auto r = client.perform(engine_op::read, buf, sizeof(buf));
            BOOST_TEST(!r.ec);
            if (test::engine_step_done(r.want))
            {
                got.append(buf, r.bytes);
            }
            else if (r.want == engine_want::input)
            {
                BOOST_TEST(fed < wire.size());
                std::size_t const put =
                    client.put_input(wire.data() + fed, wire.size() - fed);
                BOOST_TEST(put > 0);
                fed += put;
            }
        }
        // Every offered byte arrived, in order, exactly once.
        BOOST_TEST(got == msg);
        BOOST_TEST_EQ(fed, wire.size());
    }

    void
    testWriteImplicitHandshakeNeedsRead()
    {
        // WolfSSL has no renegotiation analog (the OpenSSL suite's
        // testRekeyWriteNeedsRead pins write-needs-input via TLS 1.2
        // renegotiation instead), but the same driver cell is reachable
        // directly here: wolfSSL_write implicitly drives an unfinished
        // handshake, and the first flight it stages must be flushed
        // before the wait for the peer's answer parks on input.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));

        char one = 'x';
        auto r = client.perform(engine_op::write, &one, 1);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(client.pending_output() > 0);
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);

        // Flushed and retried with nothing staged and no answer yet:
        // the write must park on input.
        r = client.perform(engine_op::write, &one, 1);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::input);
        BOOST_TEST_EQ(client.pending_output(), 0u);

        // Drive the implicit handshake to completion, then the write.
        bool wrote = false;
        for (int i = 0; i < 16 && !wrote; ++i)
        {
            auto rs = server.perform(engine_op::handshake_server, nullptr, 0);
            BOOST_TEST(!rs.ec);
            test::shuttle_bytes(server, client);
            r = client.perform(engine_op::write, &one, 1);
            BOOST_TEST(!r.ec);
            test::shuttle_bytes(client, server);
            if (test::engine_step_done(r.want))
            {
                BOOST_TEST_EQ(r.bytes, 1u);
                wrote = true;
            }
        }
        BOOST_TEST(wrote);
        std::string got;
        BOOST_TEST(test::engine_recv(server, client, got, 1));
        BOOST_TEST(got == "x");
    }

    void
    testShutdownWantWrite()
    {
        // The raw WANT_WRITE branch, distinct from SHUTDOWN_NOT_DONE's
        // "sent, awaiting the peer's" state: filling the outbound
        // staging first means our own close_notify can't even be
        // queued, so the I/O callback itself reports WANT_WRITE.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::vector<char> chunk(4096, 'x');
        std::size_t const staging_size = std::size_t{17} * 1024;
        for (int i = 0; i < 16 && client.pending_output() < staging_size; ++i)
            client.perform(engine_op::write, chunk.data(), chunk.size());
        std::size_t const filled = client.pending_output();
        BOOST_TEST(filled > 0);

        auto const r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        // Nothing new fit: the close_notify itself couldn't be queued.
        BOOST_TEST_EQ(client.pending_output(), filled);
    }

    void
    testShutdownZeroReturnNoOutput()
    {
        // Same zero-return-fatal completion as
        // testShutdownZeroReturnWithPendingOutput, but with our own
        // close_notify already drained: the verdict must be the plain
        // `done` shape, not the reporting-flush shape a still-staged
        // tail requires.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        wssl_engine client;
        wssl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        auto r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        // Draining to the peer also drains the client's own tail to zero.
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);
        BOOST_TEST_EQ(client.pending_output(), 0u);

        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(test::shuttle_bytes(server, client) > 0);

        // The "parked reader" consumes the peer's close_notify.
        char buf[64];
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(r.ec == capy::error::eof);
        BOOST_TEST(client.received_shutdown());

        // Requery: zero-return fatal with nothing staged -> plain done.
        r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::done);
        BOOST_TEST_EQ(client.pending_output(), 0u);
    }

    void
    testHandshakeFatalGarbageInput()
    {
        // A handshake fed non-TLS bytes hits a fatal decode error,
        // mirroring the OpenSSL suite's twin cell; as there, whether an
        // alert gets queued before the library gives up is a version-
        // dependent parse-state detail, so only the want/pending_output()
        // contract is portable, not one specific want.
        auto server_ctx = test::make_server_context();
        wssl_engine server;
        BOOST_TEST(
            !server.init(server_ctx, tls_role::server, std::string()));

        unsigned char garbage[64];
        for (std::size_t i = 0; i < sizeof(garbage); ++i)
            garbage[i] = static_cast<unsigned char>(i * 37 + 11);
        BOOST_TEST_EQ(
            server.put_input(garbage, sizeof(garbage)), sizeof(garbage));

        auto const r =
            server.perform(engine_op::handshake_server, nullptr, 0);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == wolfssl_category());
        BOOST_TEST_EQ(r.bytes, 0u);
        if (server.pending_output() > 0)
            BOOST_TEST(r.want == engine_want::output_then_done);
        else
            BOOST_TEST(r.want == engine_want::done);
    }

    void
    run()
    {
        testHandshake();
        testAppDataBothDirections();
        testCloseNotifySequence();
        testShutdownZeroReturnWithPendingOutput();
        testShutdownBeforeInit();
        testRekeyUpdateKeys();
        testTruncatedRecordWaitsForInput();
        testCorruptRecordMapsError();
        testPartialPutInputNoLoss();
        testWriteImplicitHandshakeNeedsRead();
        testShutdownWantWrite();
        testShutdownZeroReturnNoOutput();
        testHandshakeFatalGarbageInput();
        testGarbageDerCertificateFailsSetup();
    }

    // A certificate that does not parse in the declared format must
    // fail context setup instead of handshaking without an identity.
    void
    testGarbageDerCertificateFailsSetup()
    {
        tls_context ctx;
        // Whether the garbage surfaces here or at init() is
        // backend-dependent; the init failure below is what matters.
        std::ignore = ctx.use_certificate("\x30\x82\x00\x00", tls_file_format::der);
        std::ignore = ctx.use_private_key(test::server_key_pem, tls_file_format::pem);

        wssl_engine eng;
        // Unlike the OpenSSL engine, wolfSSL surfaces setup_error_
        // directly from init (its check_context() is a no-op).
        BOOST_TEST(!!eng.init(ctx, tls_role::server, std::string()));
    }
};

TEST_SUITE(wolfssl_engine_test, "boost.corosio.wolfssl_engine");

} // namespace boost::corosio

#endif
