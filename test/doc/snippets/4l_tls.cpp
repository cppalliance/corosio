//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4l.tls.adoc.

// Fragments deliberately leave results and bindings unused; the pages
// explain the values in prose instead.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-lambda-capture"
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(_MSC_VER)
#pragma warning(disable: 4834) // discarding [[nodiscard]] return value
#pragma warning(disable: 4189) // local variable initialized but not referenced
#pragma warning(disable: 4100) // unreferenced formal parameter
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4456) // declaration hides previous local declaration
#pragma warning(disable: 4457) // declaration hides function parameter
#pragma warning(disable: 4458) // declaration hides class member
#pragma warning(disable: 4459) // declaration hides global declaration
#endif

// tag::assume[]
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tls_context.hpp>
#include <boost/corosio/wolfssl_stream.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/ex/run_async.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
using namespace boost::corosio;
// end::assume[]

#include <boost/corosio/io/io_stream.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "test_suite.hpp"

namespace {

// Stream-wrapping fragments construct wolfssl_stream, whose symbols
// live in the separate boost_corosio_wolfssl library; they compile
// only when the build provides it.
#if defined(BOOST_COROSIO_HAS_WOLFSSL)
[[maybe_unused]] void
verified_client(corosio::io_context& ioc)
{
    // tag::verified_client[]
    tls_context ctx;
    ctx.set_default_verify_paths();              // trust the system CAs
    ctx.set_verify_mode(tls_verify_mode::peer);  // require + verify the peer

    corosio::tcp_socket sock(ioc);
    corosio::wolfssl_stream secure(&sock, ctx);
    secure.set_hostname("example.com");          // SNI + hostname check
    // end::verified_client[]
}

// Needs a live TLS peer; compiled but never executed.
[[maybe_unused]] capy::task<>
typical_flow(
    corosio::io_context& ioc,
    corosio::endpoint endpoint,
    capy::mutable_buffer buffer)
{
    // tag::typical_flow[]
    // 1. Configure a context
    tls_context ctx;
    if (auto ec = ctx.set_default_verify_paths(); ec)
        throw std::system_error(ec);
    if (auto ec = ctx.set_verify_mode(tls_verify_mode::peer); ec)
        throw std::system_error(ec);

    // 2. Connect a socket (connect() opens it automatically)
    corosio::tcp_socket sock(ioc);
    if (auto [ec] = co_await sock.connect(endpoint); ec)
        throw std::system_error(ec);

    // 3. Wrap the connected socket (pointer form; does not take ownership)
    corosio::wolfssl_stream secure(&sock, ctx);
    secure.set_hostname("api.example.com");
    if (auto [ec] = co_await secure.handshake(tls_role::client); ec)
        throw std::system_error(ec);

    // 4. Use encrypted I/O
    auto [ec, n] = co_await secure.read_some(buffer);
    // end::typical_flow[]
}
#endif

// Stand-in for certificate material obtained elsewhere (a config
// store, a secrets service, an embedded constant).
std::string load_cert_pem() { return {}; }

// Real class shown abridged; a sketch namespace avoids colliding
// with corosio::tls_stream.
namespace interface_sketch {
// tag::tls_stream_interface[]
enum class tls_role { client, server };

class tls_stream
{
public:
    template<capy::MutableBufferSequence B>
    auto read_some(B const& buffers);     // Decrypt and read

    template<capy::ConstBufferSequence B>
    auto write_some(B const& buffers);    // Encrypt and write

    virtual capy::io_task<> handshake(tls_role role) = 0;
    virtual capy::io_task<> shutdown() = 0;

    virtual capy::any_stream& next_layer() noexcept = 0;  // Underlying stream
};
// end::tls_stream_interface[]
} // namespace interface_sketch

#if defined(BOOST_COROSIO_HAS_WOLFSSL)
[[maybe_unused]] void
wolfssl_construction(corosio::io_context& ioc)
{
    // tag::wolfssl_construct[]
    #include <boost/corosio/wolfssl_stream.hpp>

    corosio::tcp_socket sock(ioc);
    // ... connect sock ...

    tls_context ctx;
    // ... configure ctx ...

    // Reference form: sock must outlive secure
    corosio::wolfssl_stream secure(&sock, ctx);

    // Or owning form: secure takes ownership of the socket
    corosio::wolfssl_stream owned(std::move(sock), ctx);
    // end::wolfssl_construct[]
}
#endif

[[maybe_unused]] void
alpn_read_back(tls_context& ctx, corosio::tls_stream& stream)
{
    // tag::alpn_config[]
    // Prefer HTTP/2, fall back to HTTP/1.1
    ctx.set_alpn({"h2", "http/1.1"});
    // ... after handshake:
    std::string_view proto = stream.alpn_protocol(); // e.g. "h2", or empty
    // end::alpn_config[]
}

[[maybe_unused]] void
hostname_verification(corosio::tls_stream& secure)
{
    // tag::set_hostname[]
    secure.set_hostname("api.example.com");
    // end::set_hostname[]
}

// Handshake fragments need a live TLS peer; compiled but never executed.
[[maybe_unused]] capy::task<>
client_handshake(corosio::tls_stream& secure)
{
    // tag::client_handshake[]
    auto [ec] = co_await secure.handshake(tls_role::client);
    if (ec)
    {
        std::cerr << "Handshake failed: " << ec.message() << "\n";
        co_return;
    }
    // end::client_handshake[]
}

[[maybe_unused]] capy::task<>
server_handshake(corosio::tls_stream& secure)
{
    // tag::server_handshake[]
    auto [ec] = co_await secure.handshake(tls_role::server);
    // end::server_handshake[]
}

[[maybe_unused]] capy::task<>
encrypted_io(corosio::tls_stream& secure)
{
    // tag::encrypted_read_write[]
    // Read encrypted data
    char buf[1024];
    auto [ec, n] = co_await secure.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));

    // Write encrypted data
    std::string msg = "Hello, TLS!";
    auto [wec, wn] = co_await secure.write_some(
        capy::const_buffer(msg.data(), msg.size()));
    // end::encrypted_read_write[]
}

[[maybe_unused]] capy::task<>
composed_io(
    corosio::tls_stream& secure,
    capy::mutable_buffer large_buffer,
    capy::const_buffer data_buffer)
{
    // tag::composed_ops[]
    // Read until buffer full
    auto [ec, n] = co_await capy::read(secure, large_buffer);

    // Write all data
    auto [wec, wn] = co_await capy::write(secure, data_buffer);
    // end::composed_ops[]
}

[[maybe_unused]] capy::task<>
tls_shutdown(corosio::tls_stream& secure, corosio::tcp_socket& sock)
{
    // tag::graceful_shutdown[]
    auto [ec] = co_await secure.shutdown();
    // Then close the underlying socket
    sock.close();
    // end::graceful_shutdown[]
}

// tag::stream_overloads[]
capy::task<void> send_request(corosio::io_stream& stream)
{
    std::string request = "GET / HTTP/1.1\r\n\r\n";
    if (auto [ec, n] = co_await capy::write(
            stream, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    char response[4096];
    co_await capy::read(
        stream, capy::mutable_buffer(response, sizeof(response)));
}

capy::task<void> send_request(corosio::tls_stream& stream)
{
    std::string request = "GET / HTTP/1.1\r\n\r\n";
    if (auto [ec, n] = co_await capy::write(
            stream, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    char response[4096];
    co_await capy::read(
        stream, capy::mutable_buffer(response, sizeof(response)));
}
// end::stream_overloads[]

#if defined(BOOST_COROSIO_HAS_WOLFSSL)
// The page shows this continuation flush-left, so the statements sit
// at column zero even though they live inside a scaffolding coroutine
// (Asciidoctor concatenates same-name tag regions).
[[maybe_unused]] capy::task<>
overload_selection(corosio::io_context& ioc, tls_context& ctx)
{
// tag::stream_overloads[]

// Plain socket uses the io_stream overload
corosio::tcp_socket sock(ioc);
co_await send_request(sock);

// TLS stream uses the tls_stream overload
corosio::wolfssl_stream secure(&sock, ctx);
co_await send_request(secure);
// end::stream_overloads[]
}

// Needs a resolvable name and a live TLS peer; compiled but never
// executed.
// tag::https_get[]
capy::task<void> https_get(
    corosio::io_context& ioc,
    std::string_view hostname,
    std::uint16_t port)
{
    // Resolve hostname
    corosio::resolver resolver(ioc);
    auto [resolve_ec, results] = co_await resolver.resolve(
        hostname, std::to_string(port));
    if (resolve_ec)
        throw std::system_error(resolve_ec);

    // Connect TCP socket (connect() opens it automatically)
    corosio::tcp_socket sock(ioc);

    for (auto const& entry : results)
    {
        auto [ec] = co_await sock.connect(entry.get_endpoint());
        if (!ec)
            break;
    }

    // Configure TLS
    tls_context ctx;
    if (auto ec = ctx.set_default_verify_paths(); ec)
        throw std::system_error(ec);
    if (auto ec = ctx.set_verify_mode(tls_verify_mode::peer); ec)
        throw std::system_error(ec);

    // Wrap the connected socket (pointer form) and handshake
    corosio::wolfssl_stream secure(&sock, ctx);
    secure.set_hostname(hostname);
    if (auto [ec] = co_await secure.handshake(tls_role::client); ec)
        throw std::system_error(ec);

    // Send HTTP request
    std::string request =
        "GET / HTTP/1.1\r\n"
        "Host: " + std::string(hostname) + "\r\n"
        "Connection: close\r\n"
        "\r\n";

    if (auto [ec, n] = co_await capy::write(
            secure, capy::const_buffer(request.data(), request.size())); ec)
        throw std::system_error(ec);

    // Read the response until EOF, one chunk at a time
    std::string response;
    for (;;)
    {
        char chunk[4096];
        auto [ec, n] = co_await capy::read(
            secure, capy::mutable_buffer(chunk, sizeof(chunk)));
        response.append(chunk, n);
        if (ec)
        {
            // EOF expected when server closes
            if (ec != capy::cond::eof)
                throw std::system_error(ec);
            break;
        }
    }

    std::cout << response << "\n";

    // Graceful shutdown
    co_await secure.shutdown();
}
// end::https_get[]

capy::task<void> handle_tls_client(
    corosio::tcp_socket sock,
    tls_context ctx);

// Binds a port and accepts forever; compiled but never executed.
// tag::tls_server[]
capy::task<void> tls_server(
    corosio::io_context& ioc,
    std::uint16_t port)
{
    // Configure server TLS context
    tls_context ctx;
    ctx.use_certificate_chain_file("server-fullchain.pem");
    ctx.use_private_key_file("server.key", tls_file_format::pem);

    // Set up acceptor
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(port));

    for (;;)
    {
        corosio::tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec) break;

        // Spawn handler
        capy::run_async(ioc.get_executor())(
            handle_tls_client(std::move(peer), ctx));
    }
}

capy::task<void> handle_tls_client(
    corosio::tcp_socket sock,
    tls_context ctx)
{
    // Owning form: the handler owns the socket, so move it in
    corosio::wolfssl_stream secure(std::move(sock), ctx);

    auto [ec] = co_await secure.handshake(tls_role::server);
    if (ec)
        co_return;

    // Handle encrypted connection...
    char buf[1024];
    auto [read_ec, n] = co_await secure.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));

    // Graceful shutdown
    co_await secure.shutdown();
}
// end::tls_server[]
#endif

// The pin the verify-callback fragment compares against; a DER
// certificate starts with the ASN.1 SEQUENCE header.
constexpr std::array<unsigned char, 3> expected_pin{0x30, 0x82, 0x03};

struct tls_test
{
    void
    testDefaultContext()
    {
        // tag::default_context[]
        // Default context (TLS 1.2+ enabled)
        tls_context ctx;
        // end::default_context[]
        BOOST_TEST(true);
    }

    void
    testLoadCertificates()
    {
        tls_context ctx;
        // tag::load_certificates[]
        // From file
        ctx.use_certificate_file("server.crt", tls_file_format::pem);

        // From memory
        std::string cert_data = load_cert_pem();
        ctx.use_certificate(cert_data, tls_file_format::pem);

        // Certificate chain (cert + intermediates)
        ctx.use_certificate_chain_file("fullchain.pem");
        // end::load_certificates[]
        BOOST_TEST(true);
    }

    void
    testLoadPrivateKeys()
    {
        tls_context ctx;
        std::string key_data = load_cert_pem();
        // tag::load_private_keys[]
        // From file
        ctx.use_private_key_file("server.key", tls_file_format::pem);

        // From memory
        ctx.use_private_key(key_data, tls_file_format::pem);
        // end::load_private_keys[]
        BOOST_TEST(true);
    }

    void
    testPasswordCallback()
    {
        tls_context ctx;
        // tag::password_callback[]
        ctx.set_password_callback(
            [](std::size_t max_len, tls_password_purpose purpose) {
                return std::string("my-key-password");
            });
        ctx.use_private_key_file("encrypted.key", tls_file_format::pem);
        // end::password_callback[]
        BOOST_TEST(true);
    }

    void
    testPkcs12()
    {
        tls_context ctx;
        // tag::pkcs12[]
        ctx.use_pkcs12_file("credentials.pfx", "bundle-password");
        // end::pkcs12[]
        BOOST_TEST(true);
    }

    void
    testSystemTrust()
    {
        tls_context ctx;
        // tag::system_trust[]
        ctx.set_default_verify_paths();
        // end::system_trust[]
        BOOST_TEST(true);
    }

    void
    testCustomCas()
    {
        tls_context ctx;
        std::string ca_pem = load_cert_pem();
        // tag::custom_cas[]
        // Single CA from memory
        ctx.add_certificate_authority(ca_pem);

        // CA file (may contain multiple certs)
        // tag::ca_bundle[]
        ctx.load_verify_file("/etc/ssl/certs/ca-certificates.crt");
        // end::ca_bundle[]

        // Directory of hashed CA files
        ctx.add_verify_path("/etc/ssl/certs");
        // end::custom_cas[]
        BOOST_TEST(true);
    }

    void
    testProtocolVersions()
    {
        tls_context ctx;
        // tag::protocol_versions[]
        // Require TLS 1.3 minimum
        ctx.set_min_protocol_version(tls_version::tls_1_3);

        // Cap at TLS 1.2 (unusual, but possible)
        ctx.set_max_protocol_version(tls_version::tls_1_2);
        // end::protocol_versions[]
        BOOST_TEST(true);
    }

    void
    testCipherSuites()
    {
        tls_context ctx;
        // tag::cipher_suites[]
        // TLS 1.2-and-below cipher list (OpenSSL syntax)
        ctx.set_ciphersuites("ECDHE+AESGCM:ECDHE+CHACHA20");
        // TLS 1.3 cipher suites (configured separately)
        ctx.set_ciphersuites_tls13("TLS_AES_256_GCM_SHA384");
        // end::cipher_suites[]
        BOOST_TEST(true);
    }

    void
    testVerifyModes()
    {
        tls_context ctx;
        // tag::verify_modes[]
        // Don't verify peer (not recommended for clients)
        ctx.set_verify_mode(tls_verify_mode::none);

        // Verify if peer presents certificate
        ctx.set_verify_mode(tls_verify_mode::peer);

        // Require peer certificate (fail if not presented)
        ctx.set_verify_mode(tls_verify_mode::require_peer);
        // end::verify_modes[]
        BOOST_TEST(!ctx.set_verify_mode(tls_verify_mode::peer));
    }

    void
    testVerifyDepth()
    {
        tls_context ctx;
        // tag::verify_depth[]
        ctx.set_verify_depth(10); // Max 10 intermediate certs
        // end::verify_depth[]
        BOOST_TEST(true);
    }

    void
    testVerifyCallback()
    {
        tls_context ctx;
        // tag::verify_callback[]
        ctx.set_verify_callback(
            [](bool preverified, corosio::verify_context& ctx) {
                if (!preverified)
                    return false;              // reject if the chain didn't verify

                auto der = ctx.certificate();  // DER of the current certificate
                return der.size() == expected_pin.size() &&
                    std::equal(der.begin(), der.end(), expected_pin.begin());
            });
        // end::verify_callback[]
        BOOST_TEST(true);
    }

    void
    testRevocation()
    {
        tls_context ctx;
        std::string crl_data = load_cert_pem();
        // tag::revocation[]
        // Load a CRL (PEM or DER), from file or memory
        ctx.add_crl_file("issuer.crl");
        ctx.add_crl(crl_data);

        // Choose how strict to be
        ctx.set_revocation_policy(tls_revocation_policy::hard_fail);
        // end::revocation[]
        BOOST_TEST(true);
    }

    void
    testMutualTlsServer()
    {
        // tag::mtls_server[]
        tls_context server_ctx;
        server_ctx.use_certificate_chain_file("server.pem");
        server_ctx.use_private_key_file("server.key", tls_file_format::pem);

        // Require client certificate
        server_ctx.set_verify_mode(tls_verify_mode::require_peer);
        server_ctx.load_verify_file("client-ca.pem");
        // end::mtls_server[]
        BOOST_TEST(!server_ctx.set_verify_mode(tls_verify_mode::require_peer));
    }

    void
    testMutualTlsClient()
    {
        // tag::mtls_client[]
        tls_context client_ctx;
        client_ctx.set_default_verify_paths();
        client_ctx.set_verify_mode(tls_verify_mode::peer);

        // Provide client certificate
        client_ctx.use_certificate_file("client.crt", tls_file_format::pem);
        client_ctx.use_private_key_file("client.key", tls_file_format::pem);
        // end::mtls_client[]
        BOOST_TEST(!client_ctx.set_verify_mode(tls_verify_mode::peer));
    }

    void
    run()
    {
        testDefaultContext();
        testLoadCertificates();
        testLoadPrivateKeys();
        testPasswordCallback();
        testPkcs12();
        testSystemTrust();
        testCustomCas();
        testProtocolVersions();
        testCipherSuites();
        testVerifyModes();
        testVerifyDepth();
        testVerifyCallback();
        testRevocation();
        testMutualTlsServer();
        testMutualTlsClient();
    }
};

} // namespace

TEST_SUITE(tls_test, "boost.corosio.doc.4l_tls");
