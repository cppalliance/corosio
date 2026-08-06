//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Transport-free unit tests for the OpenSSL record engine: two
// engines shuttle bytes in memory, so every want/ec mapping in
// `perform` is observable directly instead of through a stream.

#include <boost/corosio/tls_context.hpp>

#ifdef BOOST_COROSIO_HAS_OPENSSL

// openssl_category lives with the public stream class
#include <boost/corosio/openssl_stream.hpp>

#include "src/openssl/src/detail/engine.hpp"

// The engine header is vendor-free; this single-vendor TU pulls in
// the real headers itself for the native-handle tests (key update,
// renegotiation).
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "engine_shuttle.hpp"
#include "test_utils.hpp"
#include "test_suite.hpp"

#include <boost/capy/error.hpp>

#include <string>
#include <vector>

namespace boost::corosio {

namespace {

using detail::engine_op;
using detail::engine_want;
using ossl_engine = detail::openssl::engine;

// Init both engines from cert contexts; contexts are caller-owned
// because the engine caches the native context they carry.
bool
init_pair(
    ossl_engine& client,
    ossl_engine& server,
    tls_context const& client_ctx,
    tls_context const& server_ctx)
{
    return !client.init(client_ctx) && !server.init(server_ctx);
}

} // namespace

struct openssl_engine_test
{
    void
    testHandshake()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
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
        ossl_engine client;
        ossl_engine server;
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
        ossl_engine client;
        ossl_engine server;
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
        // shutdown parks on input (the retry protocol reaches this
        // state after every flush).
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

        // Reading again keeps reporting eof.
        r = client.perform(engine_op::read, buf, sizeof(buf));
        BOOST_TEST(r.want == engine_want::done);
        BOOST_TEST(r.ec == capy::error::eof);

        // Responding shutdown succeeds immediately (both directions
        // closed) but leaves our close_notify staged: the falsy
        // terminal must still route through the reporting flush.
        r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST(client.received_shutdown());
        BOOST_TEST(client.pending_output() > 0);

        // Writing after our own close_notify is a mapped protocol
        // error (write-side close observation; a ZERO_RETURN through
        // `write` is not constructible: OpenSSL permits half-close
        // writes after only the peer's close_notify). Our staged
        // close_notify makes this a truthy terminal WITH pending
        // output: the verdict must ask for the best-effort flush.
        r = client.perform(engine_op::write, buf, 1);
        BOOST_TEST(r.want == engine_want::output_then_done);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == openssl_category());

        // Deliver the response; the initiator's shutdown completes
        // with success and the received-shutdown bit set.
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);
        r = server.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(!r.ec);
        BOOST_TEST(test::engine_step_done(r.want));
        BOOST_TEST(server.received_shutdown());
    }

    void
    testShutdownBeforeHandshake()
    {
        // The reachable truthy shutdown terminal: shutdown before any
        // handshake is an OpenSSL-level error, mapped in one place.
        // The SYSCALL-with-empty-queue gate cannot be constructed
        // over memory BIOs, so it is not exercised here.
        auto client_ctx = test::make_client_context();
        ossl_engine client;
        BOOST_TEST(!client.init(client_ctx));
        auto const r = client.perform(engine_op::shutdown, nullptr, 0);
        BOOST_TEST(r.want == engine_want::done);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == openssl_category());
        BOOST_TEST_EQ(client.pending_output(), 0u);
    }

    void
    testRekeyKeyUpdate()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        // TLS 1.3 rekey: the KeyUpdate rides ahead of the next write.
        BOOST_TEST_EQ(
            SSL_key_update(server.native_handle(), SSL_KEY_UPDATE_REQUESTED),
            1);
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

        // The peer's read consumes the KeyUpdate and, because an
        // update was requested, stages its own KeyUpdate response:
        // output-first demands the flush ride with the data.
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
    testRekeyWriteNeedsRead()
    {
        // The previously-untestable rekey-needs-read path: a TLS 1.2
        // renegotiation driven from `write` must report `input` after
        // its flight is flushed, exactly like the read path. TLS 1.3
        // removed renegotiation and its KeyUpdate never blocks the
        // peer's write, so 1.2 is the only way to reach this mapping.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        BOOST_TEST(
            !client_ctx.set_max_protocol_version(tls_version::tls_1_2));
        BOOST_TEST(
            !server_ctx.set_max_protocol_version(tls_version::tls_1_2));
        ossl_engine client;
        ossl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
#ifdef SSL_OP_ALLOW_CLIENT_RENEGOTIATION
        // OpenSSL 3.x refuses client-initiated renegotiation unless
        // the server opts in.
        SSL_set_options(
            server.native_handle(), SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
#endif
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::string got;
        BOOST_TEST(test::engine_send(client, server, "warm"));
        BOOST_TEST(test::engine_recv(server, client, got, 4));

        BOOST_TEST_EQ(SSL_renegotiate(client.native_handle()), 1);

        // The write stages the renegotiation ClientHello and needs
        // the server's answer: flush first.
        char two[2] = {'x', 'x'};
        auto r = client.perform(engine_op::write, two, sizeof(two));
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST(test::shuttle_bytes(client, server) > 0);

        // Flushed and retried with nothing staged: the write op must
        // park on input. This is the mapping the drivers' rekey
        // comment relies on.
        r = client.perform(engine_op::write, two, sizeof(two));
        BOOST_TEST(!r.ec);
        BOOST_TEST(r.want == engine_want::input);
        BOOST_TEST_EQ(client.pending_output(), 0u);

        // Drive both sides until the write completes; the server
        // keeps reading (its renegotiation is read-driven).
        char buf[128];
        bool wrote = false;
        for (int i = 0; i < 16 && !wrote; ++i)
        {
            auto rs = server.perform(engine_op::read, buf, sizeof(buf));
            BOOST_TEST(!rs.ec);
            test::shuttle_bytes(server, client);
            r = client.perform(engine_op::write, two, sizeof(two));
            BOOST_TEST(!r.ec);
            test::shuttle_bytes(client, server);
            if (test::engine_step_done(r.want))
            {
                BOOST_TEST_EQ(r.bytes, sizeof(two));
                wrote = true;
            }
        }
        BOOST_TEST(wrote);
        BOOST_TEST(test::engine_recv(server, client, got, 2));
        BOOST_TEST(got == "xx");
    }

    void
    testTruncatedRecordWaitsForInput()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
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

        // An announced close maps the same transport eof to eof.
        BOOST_TEST(
            detail::map_fill_error(
                engine_op::read, make_error_code(capy::error::eof), true) ==
            capy::error::eof);
    }

    void
    testCorruptRecordMapsError()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
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
        BOOST_TEST(r.ec.category() == openssl_category());
        BOOST_TEST_EQ(r.bytes, 0u);
    }

    void
    testPartialPutInputNoLoss()
    {
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
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
    testWriteWantWrite()
    {
        // The raw WANT_WRITE branch (distinct from the WANT_READ-with-
        // pending-output branch that also yields output_then_retry):
        // filling the BIO pair's fixed-size buffer without draining it
        // leaves no room for even one more record, so SSL_write can't
        // stage anything and reports WANT_WRITE with zero bytes done.
        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        ossl_engine client;
        ossl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));

        std::vector<char> chunk(4096, 'x');
        detail::engine_result r{engine_want::done, {}, 0};
        int i = 0;
        for (; i < 64; ++i)
        {
            r = client.perform(
                engine_op::write, chunk.data(), chunk.size());
            BOOST_TEST(!r.ec);
            if (r.want == engine_want::output_then_retry && r.bytes == 0)
                break;
        }
        BOOST_TEST(i < 64);
        BOOST_TEST(r.want == engine_want::output_then_retry);
        BOOST_TEST_EQ(r.bytes, 0u);
        BOOST_TEST(client.pending_output() > 0);
    }

    void
    testHandshakeFatalGarbageInput()
    {
        // A handshake fed non-TLS bytes hits a fatal decode error. Which
        // OpenSSL build sends a fatal alert before giving up (queuing
        // output) versus rejects the malformed record header outright
        // (queuing nothing) depends on exactly where in the record-parse
        // state machine a given version detects the garbage, so only the
        // driver's want/pending_output() contract is portable here, not
        // one specific want.
        auto server_ctx = test::make_server_context();
        ossl_engine server;
        BOOST_TEST(!server.init(server_ctx));

        unsigned char garbage[64];
        for (std::size_t i = 0; i < sizeof(garbage); ++i)
            garbage[i] = static_cast<unsigned char>(i * 37 + 11);
        BOOST_TEST_EQ(
            server.put_input(garbage, sizeof(garbage)), sizeof(garbage));

        auto const r =
            server.perform(engine_op::handshake_server, nullptr, 0);
        BOOST_TEST(r.ec);
        BOOST_TEST(r.ec.category() == openssl_category());
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
        testShutdownBeforeHandshake();
        testRekeyKeyUpdate();
        testRekeyWriteNeedsRead();
        testWriteWantWrite();
        testHandshakeFatalGarbageInput();
        testTruncatedRecordWaitsForInput();
        testCorruptRecordMapsError();
        testPartialPutInputNoLoss();
        testDerCertificateAndKey();
        testPasswordTruncation();
        testGarbageDerCertificateFailsSetup();
    }

    // Convert a PEM cert/key fixture to DER with OpenSSL itself so the
    // engine's DER decode branch handles real input.
    static std::string
    pem_cert_to_der(char const* pem)
    {
        BIO* bio = BIO_new_mem_buf(pem, -1);
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!cert)
            return {};
        unsigned char* der = nullptr;
        int len = i2d_X509(cert, &der);
        std::string out;
        if (len > 0)
            out.assign(reinterpret_cast<char*>(der), std::size_t(len));
        OPENSSL_free(der);
        X509_free(cert);
        return out;
    }

    static std::string
    pem_key_to_der(char const* pem)
    {
        BIO* bio = BIO_new_mem_buf(pem, -1);
        EVP_PKEY* key =
            PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!key)
            return {};
        unsigned char* der = nullptr;
        int len = i2d_PrivateKey(key, &der);
        std::string out;
        if (len > 0)
            out.assign(reinterpret_cast<char*>(der), std::size_t(len));
        OPENSSL_free(der);
        EVP_PKEY_free(key);
        return out;
    }

    void
    testDerCertificateAndKey()
    {
        auto cert_der = pem_cert_to_der(test::server_cert_pem);
        auto key_der  = pem_key_to_der(test::server_key_pem);
        BOOST_TEST(!cert_der.empty());
        BOOST_TEST(!key_der.empty());

        tls_context server_ctx;
        BOOST_TEST(!server_ctx.use_certificate(
            cert_der, tls_file_format::der));
        BOOST_TEST(!server_ctx.use_private_key(
            key_der, tls_file_format::der));
        BOOST_TEST(!server_ctx.set_verify_mode(tls_verify_mode::none));

        auto client_ctx = test::make_client_context();
        ossl_engine client;
        ossl_engine server;
        BOOST_TEST(init_pair(client, server, client_ctx, server_ctx));
        BOOST_TEST(test::run_engine_handshake(client, server));
    }

    // A password longer than OpenSSL's callback buffer is truncated,
    // which then fails the key decrypt; the context build must latch
    // the failure so the driver refuses the handshake.
    void
    testPasswordTruncation()
    {
        tls_context ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_certificate(test::server_cert_pem, tls_file_format::pem);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.set_password_callback(
            [](std::size_t, tls_password_purpose) {
                return std::string(4096, 'x');
            });
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_private_key(
            test::encrypted_server_key_pem, tls_file_format::pem);

        ossl_engine eng;
        BOOST_TEST(!eng.init(ctx));
        BOOST_TEST(!!eng.check_context());
    }

    // A certificate that does not parse in the declared format must
    // fail context setup instead of handshaking without an identity.
    void
    testGarbageDerCertificateFailsSetup()
    {
        tls_context ctx;
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_certificate("\x30\x82\x00\x00", tls_file_format::der);
        // NOLINTNEXTLINE(bugprone-unused-return-value)
        ctx.use_private_key(test::server_key_pem, tls_file_format::pem);

        ossl_engine eng;
        BOOST_TEST(!eng.init(ctx));
        BOOST_TEST(!!eng.check_context());
    }
};

TEST_SUITE(openssl_engine_test, "boost.corosio.openssl_engine");

} // namespace boost::corosio

#endif
