//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// OpenSSL refusal arms in the TLS engine. The engine builds its native
// context on the first handshake and caches the outcome per
// tls_context, so every test builds a fresh context; the cache-retains-
// failure contract gets its own second handshake. The BIO ordinals are
// fixed by the engine's construction order for a given configuration.

#include <boost/corosio/detail/platform.hpp>

#if defined(COROSIO_FAULT_HAS_OPENSSL) && !defined(_WIN32)

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/openssl_stream.hpp>
#include <boost/corosio/test/mocket.hpp>
#include <boost/corosio/tls_context.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <system_error>

#include "fault.hpp"
#include "fault_test_utils.hpp"

#include "test_utils.hpp"

#include "test_suite.hpp"

namespace boost::corosio::test::fault {

namespace {

// One handshake attempt against `server_ctx` over a mocket pair;
// reports the server's error. The client uses a healthy context so a
// failure is attributable to the armed server side.
// One client context for every test, warmed below: the engine builds
// its native context on first use per tls_context object, and a fresh
// client would otherwise consume the armed ordinal with its own build.
tls_context&
warm_client_ctx()
{
    static tls_context ctx = make_client_context();
    return ctx;
}

std::error_code
server_handshake_ec(tls_context const& server_ctx)
{
    io_context ioc;
    auto [m1, m2] = corosio::test::make_mocket_pair(ioc);

    auto client = openssl_stream(&m1, warm_client_ctx());
    auto server     = openssl_stream(&m2, server_ctx);

    std::error_code server_ec;
    auto client_hs = [&]() -> capy::task<> {
        auto [ec] = co_await client.handshake(tls_role::client);
        std::ignore = ec;
        m1.close();
    };
    auto server_hs = [&]() -> capy::task<> {
        auto [ec] = co_await server.handshake(tls_role::server);
        server_ec = ec;
        m2.close();
    };
    capy::run_async(ioc.get_executor())(client_hs());
    capy::run_async(ioc.get_executor())(server_hs());
    ioc.run();
    return server_ec;
}

} // namespace

struct tls_engine_faults
{
    bool skip()
    {
        if (!hook_is_live(sys::BIO_new_mem_buf))
        {
            test_suite::log << "OpenSSL hooks not live; skipping\n";
            return true;
        }
        // Build the shared client's native context outside any armed
        // window.
        static bool warmed = [] {
            auto healthy = make_server_context();
            std::ignore  = server_handshake_ec(healthy);
            return true;
        }();
        std::ignore = warmed;
        return false;
    }

    // The credential decoder must fail the context, not crash, when
    // any of its staging BIOs cannot be made: the ordinal selects the
    // PKCS#12, entity-certificate, private-key, CA, or CRL site.
    void testBioRefusalPerSite()
    {
        if (skip())
            return;

        struct site
        {
            char const* name;
            unsigned nth;
            tls_context (*make)();
        };
        site const sites[] = {
            {"pkcs12", 1,
                [] {
                    tls_context c;
                    require_ok(c.use_pkcs12(
                        std::string_view(
                            reinterpret_cast<char const*>(server_p12),
                            sizeof(server_p12)),
                        p12_password));
                    require_ok(c.set_verify_mode(tls_verify_mode::none));
                    return c;
                }},
            {"certificate", 1, [] { return make_server_context(); }},
            {"key", 2, [] { return make_server_context(); }},
            {"ca", 3,
                [] {
                    auto c = make_server_context();
                    require_ok(c.add_certificate_authority(ca_cert_pem));
                    return c;
                }},
            {"crl", 3,
                [] {
                    auto c = make_server_context();
                    require_ok(c.add_crl(revoked_crl_pem));
                    c.set_revocation_policy(tls_revocation_policy::soft_fail);
                    return c;
                }},
        };
        for (auto const& st : sites)
        {
            auto ctx = st.make();
            fault_scope f(sys::BIO_new_mem_buf, 0, st.nth);
            auto ec = server_handshake_ec(ctx);
            test_suite::log << "bio site " << st.name << ": fired="
                            << f.fired() << " ec=" << ec.message() << "\n";
            BOOST_TEST(f.fired());
            BOOST_TEST(!!ec);
        }
    }

    // A chain certificate that cannot be duplicated must fail the
    // context rather than install a partial chain.
    void testChainDupRefusal()
    {
        if (skip())
            return;
        tls_context c;
        require_ok(c.use_pkcs12(
            std::string_view(reinterpret_cast<char const*>(server_chain_p12),
                sizeof(server_chain_p12)),
            p12_password));
        require_ok(c.set_verify_mode(tls_verify_mode::none));

        fault_scope f(sys::X509_dup, 0);
        auto ec = server_handshake_ec(c);
        BOOST_TEST(f.fired());
        BOOST_TEST(!!ec);
    }

    // A trust-store insertion that fails for a reason other than a
    // duplicate must fail the context.
    void testStoreAddRefusal()
    {
        if (skip())
            return;
        auto c = make_server_context();
        require_ok(c.add_certificate_authority(ca_cert_pem));

        fault_scope f(sys::X509_STORE_add_cert, 0);
        auto ec = server_handshake_ec(c);
        BOOST_TEST(f.fired());
        BOOST_TEST(!!ec);
    }

    // SSL_CTX_new failing poisons the cached native context: the first
    // handshake reports the failure and so must every later stream
    // built from the same tls_context, without a rebuild.
    void testContextAllocRefusalIsSticky()
    {
        if (skip())
            return;
        auto c = make_server_context();
        {
            fault_scope f(sys::SSL_CTX_new, 0);
            auto ec = server_handshake_ec(c);
            BOOST_TEST(f.fired());
            BOOST_TEST(!!ec);
        }
        // No arm: the cached failure alone must refuse the handshake.
        auto ec2 = server_handshake_ec(c);
        test_suite::log << "sticky ec2=" << ec2.message() << "\n";
        BOOST_TEST(ec2 == std::errc::not_enough_memory);
    }

    // A second handshake on a used stream resets the engine first; a
    // reset that cannot restore the session must refuse the handshake
    // rather than hand out a dead session.
    // A second handshake on a used stream resets the engine first; a
    // reset that cannot clear the session, or cannot drop it, must
    // refuse the handshake rather than hand out a dead session.
    void testSessionResetRefusal()
    {
        if (skip())
            return;
        for (sys which : {sys::SSL_clear, sys::SSL_set_session})
        {
            io_context ioc;
            auto [m1, m2]   = corosio::test::make_mocket_pair(ioc);
            auto server_ctx = make_server_context();
            auto client     = openssl_stream(&m1, warm_client_ctx());
            auto server     = openssl_stream(&m2, server_ctx);

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

            std::error_code second_ec;
            auto server_hs2 = [&]() -> capy::task<> {
                fault_scope f(which, 0);
                auto [ec] = co_await server.handshake(tls_role::server);
                second_ec = ec;
                BOOST_TEST(f.fired());
            };
            capy::run_async(ioc.get_executor())(server_hs2());
            ioc.run();
            BOOST_TEST(second_ec == std::errc::invalid_argument);
        }
    }

    // Hostname verification setup: the parameter fetch and the host
    // pinning can each refuse; a client that cannot pin the name must
    // refuse the handshake rather than proceed unverified.
    void testHostnameSetupRefusals()
    {
        if (skip())
            return;
        for (sys which : {sys::SSL_get0_param,
                 sys::X509_VERIFY_PARAM_set1_host})
        {
            io_context ioc;
            auto [m1, m2]   = corosio::test::make_mocket_pair(ioc);
            auto server_ctx = make_server_context();
            auto client     = openssl_stream(&m1, warm_client_ctx());
            auto server     = openssl_stream(&m2, server_ctx);
            client.set_hostname("localhost");

            std::error_code cec;
            auto client_hs = [&]() -> capy::task<> {
                fault_scope f(which, 0);
                auto [ec] = co_await client.handshake(tls_role::client);
                cec       = ec;
                BOOST_TEST(f.fired());
                m1.close();
            };
            auto server_hs = [&]() -> capy::task<> {
                auto [ec] = co_await server.handshake(tls_role::server);
                std::ignore = ec;
                m2.close();
            };
            capy::run_async(ioc.get_executor())(client_hs());
            capy::run_async(ioc.get_executor())(server_hs());
            ioc.run();
            BOOST_TEST(!!cec);
        }
    }

    // A staging-area refusal reads as "staging full" to the driver,
    // which must retry rather than error or hang: the read still
    // delivers the bytes once the next area request succeeds.
    void testInputStagingRefusals()
    {
        if (skip())
            return;
        for (sys which : {sys::BIO_nwrite0})
        {
            io_context ioc;
            auto [m1, m2]   = corosio::test::make_mocket_pair(ioc);
            auto server_ctx = make_server_context();
            auto client     = openssl_stream(&m1, warm_client_ctx());
            auto server     = openssl_stream(&m2, server_ctx);

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

            bool fired = false;
            std::error_code rec;
            bool done   = false;
            auto reader = [&]() -> capy::task<> {
                fault_scope f(which, 0);
                char buf[64];
                auto [ec, n] = co_await client.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                std::ignore = n;
                rec         = ec;
                fired       = f.fired();
                done        = true;
            };
            auto writer = [&]() -> capy::task<> {
                auto [ec, n] =
                    co_await server.write_some(capy::const_buffer("hi", 2));
                std::ignore = ec;
                std::ignore = n;
            };
            capy::run_async(ioc.get_executor())(reader());
            capy::run_async(ioc.get_executor())(writer());
            std::ignore = ioc.run_for(std::chrono::seconds(2));
            if (!done)
            {
                m1.close();
                m2.close();
                ioc.restart();
                ioc.run();
            }
            BOOST_TEST(done);
            BOOST_TEST(fired);
            BOOST_TEST(!rec);
        }
    }


    void run()
    {
        testBioRefusalPerSite();
        testSessionResetRefusal();
        testHostnameSetupRefusals();
        testInputStagingRefusals();
        testChainDupRefusal();
        testStoreAddRefusal();
        testContextAllocRefusalIsSticky();
    }
};

TEST_SUITE(tls_engine_faults, "boost.corosio.fault.tls");

} // namespace boost::corosio::test::fault

#endif // COROSIO_FAULT_HAS_OPENSSL
