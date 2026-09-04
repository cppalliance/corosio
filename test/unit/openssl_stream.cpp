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

// Test that header file is self-contained.
#include <boost/corosio/openssl_stream.hpp>

#include <boost/capy/read.hpp>

#include "tls_stream_tests.hpp"

#ifdef BOOST_COROSIO_HAS_OPENSSL

#include <filesystem>
#include <fstream>
#include <random>

namespace boost::corosio {

// Callable wrapper for passing to test helper templates
struct openssl_stream_factory
{
    auto operator()(tcp_socket& s, tls_context const& ctx) const
    {
        return openssl_stream(&s, ctx);
    }

    auto operator()(corosio::test::mocket& s, tls_context const& ctx) const
    {
        return openssl_stream(&s, ctx);
    }

    auto operator()(test::gated_stream& s, tls_context const& ctx) const
    {
        return openssl_stream(&s, ctx);
    }

    auto operator()(test::partial_error_stream& s, tls_context const& ctx) const
    {
        return openssl_stream(&s, ctx);
    }
};

struct openssl_stream_test
{
    static constexpr openssl_stream_factory make_stream{};

    // Context modes supported by OpenSSL (includes anon ciphers)
    static constexpr std::array<test::context_mode, 3> all_modes = {
        test::context_mode::anon, test::context_mode::shared_cert,
        test::context_mode::separate_cert};

    static constexpr std::array<test::context_mode, 2> cert_modes = {
        test::context_mode::shared_cert, test::context_mode::separate_cert};

    void testName()
    {
        using namespace test;

        io_context ioc;
        auto ctx = make_anon_context();
        tcp_socket sock(ioc);
        openssl_stream stream(&sock, ctx);

        BOOST_TEST(stream.name() == "openssl");
    }

    /** Exercise next_layer() accessors (const and non-const). */
    void testNextLayer()
    {
        using namespace test;

        io_context ioc;
        auto ctx = make_anon_context();
        tcp_socket sock(ioc);
        openssl_stream stream(&sock, ctx);

        // Non-const overload via mutable stream.
        [[maybe_unused]] capy::any_stream& mutable_next = stream.next_layer();

        // Const overload via reference to const.
        openssl_stream const& cref = stream;
        [[maybe_unused]] capy::any_stream const& const_next = cref.next_layer();

        BOOST_TEST(&mutable_next == &const_next);
    }

    /** Test that OpenSSL errors carry the OpenSSL category.

        Errors from the OpenSSL error queue must render readable messages,
        not "Unknown error <packed code>" via the system category.
    */
    void testErrorCategory()
    {
        using namespace test;

        // The category exists, is named, and decodes packed codes rather
        // than treating them as errno values.
        BOOST_TEST(openssl_category().name() ==
            std::string_view("corosio.openssl"));

        // 0x0A000086 is a packed SSL-routines error code.
        std::error_code ec(0x0A000086, openssl_category());
        std::string msg = ec.message();
        BOOST_TEST(!msg.empty());
        BOOST_TEST(msg.find("Unknown error") == std::string::npos);

        // End-to-end: a certificate-validation failure surfaces an error
        // in the OpenSSL category, not the system category.
        {
            io_context ioc;
            auto client_ctx = make_untrusted_ca_client_context();
            auto server_ctx = make_server_context();
            std::error_code client_ec;
            run_tls_test_fail(ioc, client_ctx, server_ctx, make_stream,
                make_stream, &client_ec);
            BOOST_TEST(client_ec);
            BOOST_TEST(client_ec.category() == openssl_category());
            BOOST_TEST(client_ec.message().find("Unknown error") ==
                std::string::npos);
        }
    }

    /** Test that add_verify_path() loads CAs from a hashed directory.

        OpenSSL CApath lookups require files named by subject-name hash (as
        produced by `openssl rehash`). The test CA's subject hash is a
        constant for OpenSSL 1.0.0+ (SHA-1 over the canonical subject), so
        the filename is hardcoded here to avoid linking the test against
        libcrypto. Recompute with `openssl x509 -in ca.pem -hash -noout` if
        the test certificate changes.
    */
    void testAddVerifyPath()
    {
        using namespace test;

        // A per-process-unique directory: the cxstd variants of this test
        // run concurrently, and a shared path would let one process remove
        // the CA file while another loads it, emptying the trust store and
        // failing the handshake.
        auto dir = std::filesystem::temp_directory_path() /
            ("corosio_test_capath_" +
                std::to_string(std::random_device{}()));
        std::filesystem::create_directories(dir);
        auto ca_file = dir / "d13e2296.0"; // subject hash of ca_cert_pem
        {
            std::ofstream out(ca_file, std::ios::binary);
            out << ca_cert_pem;
        }

        {
            io_context ioc;
            tls_context client_ctx;
            BOOST_TEST(!client_ctx.add_verify_path(dir.string()));
            BOOST_TEST(!client_ctx.set_verify_mode(tls_verify_mode::peer));

            auto server_ctx = make_server_context();
            run_tls_test(ioc, client_ctx, server_ctx, make_stream, make_stream);
        }

        std::filesystem::remove_all(dir);
    }

    // One handshake attempt over a mocket pair against a server
    // context configured with bad material; the deferred native-context
    // build must surface the rejection from the handshake.
    bool serverHandshakeFails(tls_context const& server_ctx)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

        auto client_ctx = test::make_client_context();
        auto client     = make_stream(m1, client_ctx);
        auto server     = make_stream(m2, server_ctx);

        bool client_done = false, server_done = false;
        std::error_code client_ec, server_ec;
        auto client_hs = [&]() -> capy::task<> {
            auto [ec]   = co_await client.handshake(tls_role::client);
            client_ec   = ec;
            client_done = true;
            m1.close();
        };
        auto server_hs = [&]() -> capy::task<> {
            auto [ec]   = co_await server.handshake(tls_role::server);
            server_ec   = ec;
            server_done = true;
            m2.close();
        };
        capy::run_async(ioc.get_executor())(client_hs());
        capy::run_async(ioc.get_executor())(server_hs());
        ioc.run();

        BOOST_TEST(client_done);
        BOOST_TEST(server_done);
        return !!client_ec || !!server_ec;
    }

    void testGarbagePkcs12FailsHandshake()
    {
        tls_context ctx;
        test::require_ok(ctx.use_pkcs12("not-pkcs12-data", "password"));
        test::require_ok(ctx.set_verify_mode(tls_verify_mode::none));
        BOOST_TEST(serverHandshakeFails(ctx));
    }

    void testGarbageCaFailsHandshake()
    {
        auto ctx = test::make_server_context();
        test::require_ok(ctx.add_certificate_authority(
            "-----BEGIN JUNK-----\nnope\n-----END JUNK-----\n"));
        BOOST_TEST(serverHandshakeFails(ctx));
    }

    void testBadCipherListFailsHandshake()
    {
        auto ctx = test::make_server_context();
        test::require_ok(ctx.set_ciphersuites("NOT-A-CIPHER"));
        BOOST_TEST(serverHandshakeFails(ctx));
    }

    void testBadTls13SuitesFailsHandshake()
    {
        auto ctx = test::make_server_context();
        test::require_ok(ctx.set_ciphersuites_tls13("garbage"));
        BOOST_TEST(serverHandshakeFails(ctx));
    }

    void testBadCrlWithRevocationFailsHandshake()
    {
        auto ctx = test::make_server_context();
        test::require_ok(ctx.add_crl("not a crl"));
        ctx.set_revocation_policy(tls_revocation_policy::soft_fail);
        BOOST_TEST(serverHandshakeFails(ctx));
    }

    void testDuplicateCaTolerated()
    {
        io_context ioc;
        auto client_ctx = test::make_client_context();
        // The store already holds this CA; the duplicate must be
        // tolerated, not fail the whole context build.
        test::require_ok(
            client_ctx.add_certificate_authority(test::ca_cert_pem));
        auto server_ctx = test::make_server_context();
        test::run_tls_test(ioc, client_ctx, server_ctx, make_stream,
            make_stream);
    }

    // Transport wrapper whose writes fail on demand and whose reads
    // can turn into a clean zero-byte EOF. Drives the driver's
    // deferred-flush-error latch and the shutdown truncation check.
    struct flush_fail_stream
    {
        corosio::test::mocket* m_;
        bool fail_writes_ = false;
        bool eof_reads_   = false;
        std::error_code inject_ec_{};

        template<class MutableBufferSequence>
        capy::io_task<std::size_t> read_some(MutableBufferSequence buffers)
        {
            if (eof_reads_)
                co_return {std::error_code{}, 0};
            co_return co_await m_->read_some(buffers);
        }

        template<class ConstBufferSequence>
        capy::io_task<std::size_t> write_some(ConstBufferSequence buffers)
        {
            if (fail_writes_)
                co_return {inject_ec_, 0};
            co_return co_await m_->write_some(buffers);
        }
    };

    // Handshake a client/server pair over mockets, wrapping the client
    // transport, then hand control to `scenario`.
    template<class Wrapper, class Scenario>
    static void runWrappedSession(Wrapper& w, Scenario scenario)
    {
        io_context ioc;
        auto [m1, m2] = corosio::test::make_mocket_pair(ioc);
        w.m_ = &m1;

        auto client_ctx = test::make_client_context();
        auto server_ctx = test::make_server_context();
        auto client     = openssl_stream(&w, client_ctx);
        auto server     = openssl_stream(&m2, server_ctx);

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
        scenario(ioc, client, server, m1, m2);
    }

    void testZeroLengthBufferInSequence()
    {
        flush_fail_stream w{};
        runWrappedSession(w, [](io_context& ioc, auto& client, auto& server,
                                 auto&, auto&) {
            char rx[16] = {};
            bool wrote = false, read = false;
            auto writer = [&]() -> capy::task<> {
                std::array<capy::const_buffer, 2> bufs = {
                    capy::const_buffer("", 0), capy::const_buffer("hey", 3)};
                auto [ec, n] = co_await client.write_some(bufs);
                wrote        = !ec && n == 3;
            };
            auto reader = [&]() -> capy::task<> {
                std::array<capy::mutable_buffer, 2> bufs = {
                    capy::mutable_buffer(rx, 0),
                    capy::mutable_buffer(rx, sizeof(rx))};
                auto [ec, n] = co_await server.read_some(bufs);
                read         = !ec && n == 3;
            };
            capy::run_async(ioc.get_executor())(writer());
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            BOOST_TEST(wrote);
            BOOST_TEST(read);
            BOOST_TEST_EQ(std::string_view(rx, 3), "hey");
        });
    }

    void testWriteFlushErrorIsLatched()
    {
        flush_fail_stream w{};
        w.inject_ec_ = std::make_error_code(std::errc::connection_reset);
        runWrappedSession(w, [&w](io_context& ioc, auto& client, auto&,
                                   auto&, auto&) {
            // The engine accepts the whole payload, so the failed
            // transport flush must be deferred to the next operation,
            // not conflated with this one's success.
            bool first_ok = false;
            std::error_code second_ec;
            auto writer = [&]() -> capy::task<> {
                w.fail_writes_ = true;
                auto [ec, n]   = co_await client.write_some(
                    capy::const_buffer("hello", 5));
                first_ok = !ec && n == 5;
                auto [ec2, n2] =
                    co_await client.write_some(capy::const_buffer("x", 1));
                std::ignore = n2;
                second_ec   = ec2;
            };
            capy::run_async(ioc.get_executor())(writer());
            ioc.run();
            BOOST_TEST(first_ok);
            BOOST_TEST(second_ec ==
                       std::make_error_code(std::errc::connection_reset));
        });
    }

    void testCorruptRecordFailsReadAndShutdown()
    {
        flush_fail_stream w{};
        runWrappedSession(w, [](io_context& ioc, auto& client, auto&,
                                 auto&, auto& m2) {
            // Raw junk on the transport: the engine rejects the record
            // and queues a fatal alert the driver must still flush.
            char junk[64];
            for (std::size_t i = 0; i < sizeof(junk); ++i)
                junk[i] = static_cast<char>(0x5a ^ i);
            char rx[16];
            std::error_code rec;
            bool shut_done = false;
            auto peer = [&]() -> capy::task<> {
                auto [ec, n] = co_await m2.write_some(
                    capy::const_buffer(junk, sizeof(junk)));
                std::ignore = ec;
                std::ignore = n;
            };
            auto reader = [&]() -> capy::task<> {
                auto [ec, n] =
                    co_await client.read_some(capy::mutable_buffer(rx, sizeof(rx)));
                std::ignore = n;
                rec         = ec;
                auto [sec] = co_await client.shutdown();
                std::ignore = sec;
                shut_done   = true;
            };
            capy::run_async(ioc.get_executor())(peer());
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            BOOST_TEST(!!rec);
            BOOST_TEST(shut_done);
        });
    }

    void testOversizedWriteRoundTrips()
    {
        flush_fail_stream w{};
        runWrappedSession(w, [](io_context& ioc, auto& client, auto& server,
                                 auto&, auto&) {
            // Larger than the engine's staging capacity: the driver
            // must flush and retry until the payload is accepted.
            std::string const payload(64 * 1024, 'q');
            std::string rx;
            bool wrote = false, read = false;
            auto writer = [&]() -> capy::task<> {
                auto [ec, n] = co_await capy::write(client,
                    capy::const_buffer(payload.data(), payload.size()));
                wrote = !ec && n == payload.size();
            };
            auto reader = [&]() -> capy::task<> {
                rx.resize(payload.size());
                auto [ec, n] = co_await capy::read(server,
                    capy::mutable_buffer(rx.data(), rx.size()));
                read = !ec && n == rx.size();
            };
            capy::run_async(ioc.get_executor())(writer());
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            BOOST_TEST(wrote);
            BOOST_TEST(read);
            BOOST_TEST(rx == payload);
        });
    }


    void testShutdownOnDeadTransportReportsTruncation()
    {
        flush_fail_stream w{};
        runWrappedSession(w, [&w](io_context& ioc, auto& client, auto&,
                                   auto& m1, auto& m2) {
            // The peer vanishes without a close_notify: the transport
            // reads clean EOF, and the driver must report the
            // truncation on shutdown rather than a clean close.
            w.eof_reads_ = true;
            std::ignore  = m1;
            std::ignore  = m2;
            std::error_code sec;
            bool done     = false;
            auto shutter  = [&]() -> capy::task<> {
                auto [ec] = co_await client.shutdown();
                sec       = ec;
                done      = true;
            };
            capy::run_async(ioc.get_executor())(shutter());
            ioc.run();
            BOOST_TEST(done);
            BOOST_TEST(!!sec);
        });
    }


    void run()
    {
        test::testIoBeforeHandshake(make_stream);
        test::testHandshakeFuse(make_stream);
        test::testReadWriteFuse(make_stream);
        test::testShutdownFuse(make_stream);
        test::testSuccessCases(make_stream, all_modes);
        test::testFailureCases(make_stream);
        test::testTlsShutdown(make_stream, cert_modes);
        test::testStreamTruncated(make_stream, cert_modes);
        test::testStopTokenCancellation(make_stream);
        test::testShutdownCancel(make_stream);
        test::testSocketErrorPropagation(make_stream);
        test::testCertificateValidation(make_stream);
        test::testSni(make_stream);
        test::testSniCallback(make_stream);
        test::testHostnamePersistence(make_stream);
        test::testHostnameRedirect(make_stream);
        test::testHostnameClear(make_stream);
        test::testFullDuplex(make_stream);
        test::testFullDuplexBulk(make_stream);
        test::testRecordBoundaryTransfer(make_stream);
        test::testShutdownOverRead(make_stream);
        test::testShutdownSimultaneousClose(make_stream);
        test::testPartialReadWithError(make_stream);
        test::testCancelParkedReader(make_stream);
        test::testFullDuplexMtStrand(make_stream);
        test::testDeferredFlushError(make_stream);
        test::testTlsLifecycleEdges(make_stream);
        test::testShutdownTruncation(make_stream);
        test::testHostnameRetryAfterFailure(make_stream);
        test::testHostnameIpLiteral(make_stream, /*ip_supported=*/true);
        test::testAlpnAccessorEmpty(make_stream);
        test::testAlpn(make_stream, /*alpn_supported=*/true);
        test::testAlpnNoOverlap(make_stream, /*alpn_supported=*/true);
        test::testProtocolVersion(make_stream);
        test::testCiphersuitesTls13(
            make_stream, "TLS_AES_128_GCM_SHA256", "TLS_AES_256_GCM_SHA384");
        test::testPkcs12(make_stream);
        test::testPkcs12Chain(make_stream);
        test::testCertificateChain(make_stream);
        test::testDefaultVerifyPaths(make_stream);
        test::testCrlRevocation(make_stream, /*crl_supported=*/true);
        test::testVerifyCallback(make_stream);
        test::testVerifyCallbackOnSuccess(make_stream);
        test::testMtls(make_stream);
        test::testMoveSemantics(make_stream);
        test::testAbruptClose(make_stream);
        test::testEncryptedKey(make_stream);
        test::testInvalidContextHandshake(make_stream);
        testGarbagePkcs12FailsHandshake();
        testGarbageCaFailsHandshake();
        testBadCipherListFailsHandshake();
        testBadTls13SuitesFailsHandshake();
        testBadCrlWithRevocationFailsHandshake();
        testDuplicateCaTolerated();
        testZeroLengthBufferInSequence();
        testWriteFlushErrorIsLatched();
        testCorruptRecordFailsReadAndShutdown();
        testOversizedWriteRoundTrips();
        testShutdownOnDeadTransportReportsTruncation();

        test::testReset(make_stream, cert_modes);
        test::testResetViaHandshake(make_stream, cert_modes);
        test::testResetFuse(make_stream);

        testErrorCategory();
        testAddVerifyPath();
        testName();
        testNextLayer();
    }
};

TEST_SUITE(openssl_stream_test, "boost.corosio.openssl_stream");

} // namespace boost::corosio

#endif
