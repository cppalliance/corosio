//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tls_context.hpp>

// Private impl header provides tls_context_data definition for inspection.
#include "src/corosio/src/tls/detail/context_impl.hpp"

#include "test_utils.hpp"
#include "test_suite.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace boost::corosio {

namespace {

// RAII helper that creates a temp file with given contents and removes it.
struct temp_file
{
    std::filesystem::path path;

    temp_file(std::string_view prefix, std::string_view contents)
    {
        path = std::filesystem::temp_directory_path()
             / (std::string(prefix) + std::to_string(std::rand()));
        std::ofstream ofs(path, std::ios::binary);
        ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    ~temp_file()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    temp_file(temp_file const&) = delete;
    temp_file& operator=(temp_file const&) = delete;

    std::string str() const { return path.string(); }
};

// A path guaranteed not to exist.
constexpr char const* nonexistent_path =
    "/tmp/corosio_no_such_file_zzz_99999999.pem";

} // namespace

struct tls_context_test
{
    void testDefaultConstruction()
    {
        tls_context ctx;
        auto const& data = detail::get_tls_context_data(ctx);

        BOOST_TEST_EQ(data.entity_certificate, std::string());
        BOOST_TEST(data.entity_cert_format == tls_file_format::pem);
        BOOST_TEST(data.min_version == tls_version::tls_1_2);
        BOOST_TEST(data.max_version == tls_version::tls_1_3);
        BOOST_TEST(data.verification_mode == tls_verify_mode::none);
        BOOST_TEST_EQ(data.verify_depth, 100);
        BOOST_TEST(!data.use_default_verify_paths);
        BOOST_TEST(!data.require_ocsp_staple);
        BOOST_TEST(data.revocation == tls_revocation_policy::disabled);
    }

    void testCopyAndMove()
    {
        tls_context a;
        BOOST_TEST(!a.use_certificate(test::server_cert_pem,
                                      tls_file_format::pem));

        // Copy: shares state
        tls_context b(a);
        auto const& bd = detail::get_tls_context_data(b);
        BOOST_TEST_EQ(bd.entity_certificate, std::string(test::server_cert_pem));

        // Copy-assign
        tls_context c;
        c = a;
        auto const& cd = detail::get_tls_context_data(c);
        BOOST_TEST_EQ(cd.entity_certificate, std::string(test::server_cert_pem));

        // Move-construct
        tls_context d(std::move(b));
        auto const& dd = detail::get_tls_context_data(d);
        BOOST_TEST_EQ(dd.entity_certificate, std::string(test::server_cert_pem));

        // Move-assign
        tls_context e;
        e = std::move(c);
        auto const& ed = detail::get_tls_context_data(e);
        BOOST_TEST_EQ(ed.entity_certificate, std::string(test::server_cert_pem));
    }

    //
    // Credential loading (in-memory)
    //

    void testUseCertificateInMemory()
    {
        tls_context ctx;
        auto ec = ctx.use_certificate(
            test::server_cert_pem, tls_file_format::pem);
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.entity_certificate,
                      std::string(test::server_cert_pem));
        BOOST_TEST(data.entity_cert_format == tls_file_format::pem);

        // DER format selector should also be stored without parsing
        tls_context der_ctx;
        ec = der_ctx.use_certificate("\x30\x82\x00\x00", tls_file_format::der);
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(der_ctx).entity_cert_format
                   == tls_file_format::der);
    }

    void testUseCertificateChainInMemory()
    {
        tls_context ctx;
        auto ec = ctx.use_certificate_chain(test::server_fullchain_pem);
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.certificate_chain,
                      std::string(test::server_fullchain_pem));
    }

    void testUsePrivateKeyInMemory()
    {
        tls_context ctx;
        auto ec = ctx.use_private_key(
            test::server_key_pem, tls_file_format::pem);
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.private_key, std::string(test::server_key_pem));
        BOOST_TEST(data.private_key_format == tls_file_format::pem);

        // Garbage PEM is accepted at storage time (no parsing in tls_context)
        tls_context bogus;
        ec = bogus.use_private_key(
            "-----BEGIN PRIVATE KEY-----\nnot-base64!@#\n",
            tls_file_format::pem);
        BOOST_TEST(!ec);
    }

    //
    // Credential loading (from file)
    //

    void testUseCertificateFile()
    {
        // Success: read existing temp file
        {
            temp_file f("corosio_cert_", test::server_cert_pem);
            tls_context ctx;
            auto ec = ctx.use_certificate_file(f.str(), tls_file_format::pem);
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(detail::get_tls_context_data(ctx).entity_certificate,
                          std::string(test::server_cert_pem));
        }

        // Failure: nonexistent file -> ENOENT
        {
            tls_context ctx;
            auto ec = ctx.use_certificate_file(
                nonexistent_path, tls_file_format::pem);
            BOOST_TEST(ec);
            BOOST_TEST_EQ(ec.value(), ENOENT);
        }
    }

    void testUseCertificateChainFile()
    {
        // Success
        {
            temp_file f("corosio_chain_", test::server_fullchain_pem);
            tls_context ctx;
            auto ec = ctx.use_certificate_chain_file(f.str());
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(detail::get_tls_context_data(ctx).certificate_chain,
                          std::string(test::server_fullchain_pem));
        }

        // Missing file
        {
            tls_context ctx;
            auto ec = ctx.use_certificate_chain_file(nonexistent_path);
            BOOST_TEST(ec);
            BOOST_TEST_EQ(ec.value(), ENOENT);
        }
    }

    void testUsePrivateKeyFile()
    {
        // Success
        {
            temp_file f("corosio_key_", test::server_key_pem);
            tls_context ctx;
            auto ec = ctx.use_private_key_file(f.str(), tls_file_format::pem);
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(detail::get_tls_context_data(ctx).private_key,
                          std::string(test::server_key_pem));
        }

        // Missing file
        {
            tls_context ctx;
            auto ec = ctx.use_private_key_file(
                nonexistent_path, tls_file_format::pem);
            BOOST_TEST(ec);
            BOOST_TEST_EQ(ec.value(), ENOENT);
        }
    }

    //
    // PKCS#12 (currently unsupported)
    //

    void testPkcs12Unsupported()
    {
        tls_context ctx;
        auto ec = ctx.use_pkcs12("not-pkcs12-data", "password");
        BOOST_TEST(ec);
        BOOST_TEST(ec == std::make_error_code(std::errc::function_not_supported));

        ec = ctx.use_pkcs12_file("/some/path", "password");
        BOOST_TEST(ec);
        BOOST_TEST(ec == std::make_error_code(std::errc::function_not_supported));
    }

    //
    // Trust anchors
    //

    void testAddCertificateAuthority()
    {
        tls_context ctx;
        auto ec = ctx.add_certificate_authority(test::ca_cert_pem);
        BOOST_TEST(!ec);
        ec = ctx.add_certificate_authority(test::root_ca_cert_pem);
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.ca_certificates.size(), 2u);
        BOOST_TEST_EQ(data.ca_certificates[0], std::string(test::ca_cert_pem));
        BOOST_TEST_EQ(data.ca_certificates[1],
                      std::string(test::root_ca_cert_pem));
    }

    void testLoadVerifyFile()
    {
        // Success
        {
            temp_file f("corosio_ca_", test::root_ca_cert_pem);
            tls_context ctx;
            auto ec = ctx.load_verify_file(f.str());
            BOOST_TEST(!ec);
            auto const& data = detail::get_tls_context_data(ctx);
            BOOST_TEST_EQ(data.ca_certificates.size(), 1u);
            BOOST_TEST_EQ(data.ca_certificates[0],
                          std::string(test::root_ca_cert_pem));
        }

        // Missing file
        {
            tls_context ctx;
            auto ec = ctx.load_verify_file(nonexistent_path);
            BOOST_TEST(ec);
            BOOST_TEST_EQ(ec.value(), ENOENT);
        }
    }

    void testVerifyPaths()
    {
        tls_context ctx;
        auto ec = ctx.add_verify_path("/etc/ssl/certs");
        BOOST_TEST(!ec);
        ec = ctx.add_verify_path("/usr/local/etc/ssl");
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.verify_paths.size(), 2u);
        BOOST_TEST_EQ(data.verify_paths[0], std::string("/etc/ssl/certs"));
        BOOST_TEST_EQ(data.verify_paths[1], std::string("/usr/local/etc/ssl"));
    }

    void testDefaultVerifyPaths()
    {
        tls_context ctx;
        BOOST_TEST(!detail::get_tls_context_data(ctx).use_default_verify_paths);

        auto ec = ctx.set_default_verify_paths();
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(ctx).use_default_verify_paths);
    }

    //
    // Protocol configuration
    //

    void testProtocolVersionRoundTrip()
    {
        tls_context ctx;

        auto ec = ctx.set_min_protocol_version(tls_version::tls_1_3);
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(ctx).min_version
                   == tls_version::tls_1_3);

        ec = ctx.set_max_protocol_version(tls_version::tls_1_2);
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(ctx).max_version
                   == tls_version::tls_1_2);

        // Reset min/max
        ec = ctx.set_min_protocol_version(tls_version::tls_1_2);
        BOOST_TEST(!ec);
        ec = ctx.set_max_protocol_version(tls_version::tls_1_3);
        BOOST_TEST(!ec);
    }

    void testCiphersuites()
    {
        tls_context ctx;
        auto ec = ctx.set_ciphersuites("ECDHE+AESGCM");
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).ciphersuites,
                      std::string("ECDHE+AESGCM"));

        // Empty string is accepted at storage time
        ec = ctx.set_ciphersuites("");
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).ciphersuites,
                      std::string());
    }

    void testAlpn()
    {
        tls_context ctx;

        // Initially empty
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).alpn_protocols.size(),
                      0u);

        // Set list
        auto ec = ctx.set_alpn({"h2", "http/1.1"});
        BOOST_TEST(!ec);
        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.alpn_protocols.size(), 2u);
        BOOST_TEST_EQ(data.alpn_protocols[0], std::string("h2"));
        BOOST_TEST_EQ(data.alpn_protocols[1], std::string("http/1.1"));

        // Reset replaces the list
        ec = ctx.set_alpn({"http/1.1"});
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(data.alpn_protocols.size(), 1u);
        BOOST_TEST_EQ(data.alpn_protocols[0], std::string("http/1.1"));

        // Clear by passing empty list
        ec = ctx.set_alpn({});
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(data.alpn_protocols.size(), 0u);
    }

    //
    // Verification
    //

    void testVerifyModeAndDepth()
    {
        tls_context ctx;

        auto ec = ctx.set_verify_mode(tls_verify_mode::peer);
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(ctx).verification_mode
                   == tls_verify_mode::peer);

        ec = ctx.set_verify_mode(tls_verify_mode::require_peer);
        BOOST_TEST(!ec);
        BOOST_TEST(detail::get_tls_context_data(ctx).verification_mode
                   == tls_verify_mode::require_peer);

        ec = ctx.set_verify_depth(5);
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).verify_depth, 5);
    }

    void testHostname()
    {
        tls_context ctx;
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).hostname, std::string());

        ctx.set_hostname("api.example.com");
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).hostname,
                      std::string("api.example.com"));

        // Overwrite
        ctx.set_hostname("other.example.com");
        BOOST_TEST_EQ(detail::get_tls_context_data(ctx).hostname,
                      std::string("other.example.com"));
    }

    void testServernameCallback()
    {
        tls_context ctx;

        bool invoked = false;
        std::string saw;
        ctx.set_servername_callback(
            [&](std::string_view name) -> bool {
                invoked = true;
                saw     = std::string(name);
                return name == "ok.example.com";
            });

        auto const& cb = detail::get_tls_context_data(ctx).servername_callback;
        BOOST_TEST(static_cast<bool>(cb));

        // Invoke through stored std::function
        BOOST_TEST(cb("ok.example.com"));
        BOOST_TEST(invoked);
        BOOST_TEST_EQ(saw, std::string("ok.example.com"));

        BOOST_TEST(!cb("nope.example.com"));
    }

    void testPasswordCallback()
    {
        tls_context ctx;

        bool invoked = false;
        ctx.set_password_callback(
            [&](std::size_t max_len, tls_password_purpose purpose)
                -> std::string {
                invoked = true;
                BOOST_TEST(max_len > 0);
                BOOST_TEST(purpose == tls_password_purpose::for_reading
                           || purpose == tls_password_purpose::for_writing);
                return std::string("secret");
            });

        auto const& cb = detail::get_tls_context_data(ctx).password_callback;
        BOOST_TEST(static_cast<bool>(cb));
        auto pw = cb(64, tls_password_purpose::for_reading);
        BOOST_TEST_EQ(pw, std::string("secret"));
        BOOST_TEST(invoked);
    }

    //
    // Revocation
    //

    void testAddCrl()
    {
        tls_context ctx;
        auto ec = ctx.add_crl("-----BEGIN X509 CRL-----\nABC\n-----END X509 CRL-----\n");
        BOOST_TEST(!ec);
        ec = ctx.add_crl("second-crl-data");
        BOOST_TEST(!ec);

        auto const& data = detail::get_tls_context_data(ctx);
        BOOST_TEST_EQ(data.crls.size(), 2u);
    }

    void testAddCrlFile()
    {
        // Success
        {
            temp_file f("corosio_crl_", "fake-crl-bytes");
            tls_context ctx;
            auto ec = ctx.add_crl_file(f.str());
            BOOST_TEST(!ec);
            auto const& data = detail::get_tls_context_data(ctx);
            BOOST_TEST_EQ(data.crls.size(), 1u);
            BOOST_TEST_EQ(data.crls[0], std::string("fake-crl-bytes"));
        }

        // Missing file
        {
            tls_context ctx;
            auto ec = ctx.add_crl_file(nonexistent_path);
            BOOST_TEST(ec);
            BOOST_TEST_EQ(ec.value(), ENOENT);
        }
    }

    void testOcspStaple()
    {
        tls_context ctx;
        auto ec = ctx.set_ocsp_staple("\x30\x82\x01\x00 binary ocsp blob");
        BOOST_TEST(!ec);
        BOOST_TEST(!detail::get_tls_context_data(ctx).ocsp_staple.empty());
    }

    void testRequireOcspStaple()
    {
        tls_context ctx;
        BOOST_TEST(!detail::get_tls_context_data(ctx).require_ocsp_staple);

        ctx.set_require_ocsp_staple(true);
        BOOST_TEST(detail::get_tls_context_data(ctx).require_ocsp_staple);

        ctx.set_require_ocsp_staple(false);
        BOOST_TEST(!detail::get_tls_context_data(ctx).require_ocsp_staple);
    }

    void testRevocationPolicy()
    {
        tls_context ctx;
        ctx.set_revocation_policy(tls_revocation_policy::soft_fail);
        BOOST_TEST(detail::get_tls_context_data(ctx).revocation
                   == tls_revocation_policy::soft_fail);

        ctx.set_revocation_policy(tls_revocation_policy::hard_fail);
        BOOST_TEST(detail::get_tls_context_data(ctx).revocation
                   == tls_revocation_policy::hard_fail);

        ctx.set_revocation_policy(tls_revocation_policy::disabled);
        BOOST_TEST(detail::get_tls_context_data(ctx).revocation
                   == tls_revocation_policy::disabled);
    }

    void run()
    {
        testDefaultConstruction();
        testCopyAndMove();

        testUseCertificateInMemory();
        testUseCertificateChainInMemory();
        testUsePrivateKeyInMemory();
        testUseCertificateFile();
        testUseCertificateChainFile();
        testUsePrivateKeyFile();
        testPkcs12Unsupported();

        testAddCertificateAuthority();
        testLoadVerifyFile();
        testVerifyPaths();
        testDefaultVerifyPaths();

        testProtocolVersionRoundTrip();
        testCiphersuites();
        testAlpn();

        testVerifyModeAndDepth();
        testHostname();
        testServernameCallback();
        testPasswordCallback();

        testAddCrl();
        testAddCrlFile();
        testOcspStaple();
        testRequireOcspStaple();
        testRevocationPolicy();
    }
};

TEST_SUITE(tls_context_test, "boost.corosio.tls_context");

} // namespace boost::corosio
