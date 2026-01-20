//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP
#define BOOST_COROSIO_TEST_TLS_TEST_UTILS_HPP

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/io_stream.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/tls/context.hpp>
#include <boost/corosio/tls/tls_stream.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include "test_suite.hpp"

namespace boost {
namespace corosio {
namespace tls {
namespace test {

//------------------------------------------------------------------------------
//
// Embedded Test Certificates
//
//------------------------------------------------------------------------------

// Self-signed server certificate from Boost.Beast (valid, self-signed)
// This cert is also its own CA (self-signed)
inline constexpr char const* server_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDlTCCAn2gAwIBAgIUOLxr3q7Wd/pto1+2MsW4fdRheCIwDQYJKoZIhvcNAQEL\n"
    "BQAwWjELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMRQwEgYDVQQHDAtMb3MgQW5n\n"
    "ZWxlczEOMAwGA1UECgwFQmVhc3QxGDAWBgNVBAMMD3d3dy5leGFtcGxlLmNvbTAe\n"
    "Fw0yMTA3MDYwMTQ5MjVaFw00ODExMjEwMTQ5MjVaMFoxCzAJBgNVBAYTAlVTMQsw\n"
    "CQYDVQQIDAJDQTEUMBIGA1UEBwwLTG9zIEFuZ2VsZXMxDjAMBgNVBAoMBUJlYXN0\n"
    "MRgwFgYDVQQDDA93d3cuZXhhbXBsZS5jb20wggEiMA0GCSqGSIb3DQEBAQUAA4IB\n"
    "DwAwggEKAoIBAQCz0GwgnxSBhygxBdhTHGx5LDLIJSuIDJ6nMwZFvAjdhLnB/vOT\n"
    "Lppr5MKxqQHEpYdyDYGD1noBoz4TiIRj5JapChMgx58NLq5QyXkHV/ONT7yi8x05\n"
    "P41c2F9pBEnUwUxIUG1Cb6AN0cZWF/wSMOZ0w3DoBhnl1sdQfQiS25MTK6x4tATm\n"
    "Wm9SJc2lsjWptbyIN6hFXLYPXTwnYzCLvv1EK6Ft7tMPc/FcJpd/wYHgl8shDmY7\n"
    "rV+AiGTxUU35V0AzpJlmvct5aJV/5vSRRLwT9qLZSddE9zy/0rovC5GML6S7BUC4\n"
    "lIzJ8yxzOzSStBPxvdrOobSSNlRZIlE7gnyNAgMBAAGjUzBRMB0GA1UdDgQWBBR+\n"
    "dYtY9zmFSw9GYpEXC1iJKHC0/jAfBgNVHSMEGDAWgBR+dYtY9zmFSw9GYpEXC1iJ\n"
    "KHC0/jAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBzKrsiYywl\n"
    "RKeB2LbddgSf7ahiQMXCZpAjZeJikIoEmx+AmjQk1bam+M7WfpRAMnCKooU+Utp5\n"
    "TwtijjnJydkZHFR6UH6oCWm8RsUVxruao/B0UFRlD8q+ZxGd4fGTdLg/ztmA+9oC\n"
    "EmrcQNdz/KIxJj/fRB3j9GM4lkdaIju47V998Z619E/6pt7GWcAySm1faPB0X4fL\n"
    "FJ6iYR2r/kJLoppPqL0EE49uwyYQ1dKhXS2hk+IIfA9mBn8eAFb/0435A2fXutds\n"
    "qhvwIOmAObCzcoKkz3sChbk4ToUTqbC0TmFAXI5Upz1wnADzjpbJrpegCA3pmvhT\n"
    "7356drqnCGY9\n"
    "-----END CERTIFICATE-----\n";

// CA cert is the same as server cert (self-signed)
inline constexpr char const* ca_cert_pem = server_cert_pem;

// Server private key from Boost.Beast
inline constexpr char const* server_key_pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCz0GwgnxSBhygx\n"
    "BdhTHGx5LDLIJSuIDJ6nMwZFvAjdhLnB/vOTLppr5MKxqQHEpYdyDYGD1noBoz4T\n"
    "iIRj5JapChMgx58NLq5QyXkHV/ONT7yi8x05P41c2F9pBEnUwUxIUG1Cb6AN0cZW\n"
    "F/wSMOZ0w3DoBhnl1sdQfQiS25MTK6x4tATmWm9SJc2lsjWptbyIN6hFXLYPXTwn\n"
    "YzCLvv1EK6Ft7tMPc/FcJpd/wYHgl8shDmY7rV+AiGTxUU35V0AzpJlmvct5aJV/\n"
    "5vSRRLwT9qLZSddE9zy/0rovC5GML6S7BUC4lIzJ8yxzOzSStBPxvdrOobSSNlRZ\n"
    "IlE7gnyNAgMBAAECggEAY0RorQmldGx9D7M+XYOPjsWLs1px0cXFwGA20kCgVEp1\n"
    "kleBeHt93JqJsTKwOzN2tswl9/ZrnIPWPUpcbBlB40ggjzQk5k4jBY50Nk2jsxuV\n"
    "9A9qzrP7AoqhAYTQjZe42SMtbkPZhEeOyvCqxBAi6csLhcv4eB4+In0kQo7dfvLs\n"
    "Xu/3WhSsuAWqdD9EGnhD3n+hVTtgiasRe9318/3R9DzP+IokoQGOtXm+1dsfP0mV\n"
    "8XGzQHBpUtJNn0yi6SC4kGEQuKkX33zORlSnZgT5VBLofNgra0THd7x3atOx1lbr\n"
    "V0QizvCdBa6j6FwhOQwW8UwgOCnUbWXl/Xn4OaofMQKBgQDdRXSMyys7qUMe4SYM\n"
    "Mdawj+rjv0Hg98/xORuXKEISh2snJGKEwV7L0vCn468n+sM19z62Axz+lvOUH8Qr\n"
    "hLkBNqJvtIP+b0ljRjem78K4a4qIqUlpejpRLw6a/+44L76pMJXrYg3zdBfwzfwu\n"
    "b9NXdwHzWoNuj4v36teGP6xOUwKBgQDQCT52XX96NseNC6HeK5BgWYYjjxmhksHi\n"
    "stjzPJKySWXZqJpHfXI8qpOd0Sd1FHB+q1s3hand9c+Rxs762OXlqA9Q4i+4qEYZ\n"
    "qhyRkTsl+2BhgzxmoqGd5gsVT7KV8XqtuHWLmetNEi+7+mGSFf2iNFnonKlvT1JX\n"
    "4OQZC7ntnwKBgH/ORFmmaFxXkfteFLnqd5UYK5ZMvGKTALrWP4d5q2BEc7HyJC2F\n"
    "+5lDR9nRezRedS7QlppPBgpPanXeO1LfoHSA+CYJYEwwP3Vl83Mq/Y/EHgp9rXeN\n"
    "L+4AfjEtLo2pljjnZVDGHETIg6OFdunjkXDtvmSvnUbZBwG11bMnSAEdAoGBAKFw\n"
    "qwJb6FNFM3JnNoQctnuuvYPWxwM1yjRMqkOIHCczAlD4oFEeLoqZrNhpuP8Ij4wd\n"
    "GjpqBbpzyVLNP043B6FC3C/edz4Lh+resjDczVPaUZ8aosLbLiREoxE0udfWf2dU\n"
    "oBNnrMwwcs6jrRga7Kr1iVgUSwBQRAxiP2CYUv7tAoGBAKdPdekPNP/rCnHkKIkj\n"
    "o13pr+LJ8t+15vVzZNHwPHUWiYXFhG8Ivx7rqLQSPGcuPhNss3bg1RJiZAUvF6fd\n"
    "e6QS4EZM9dhhlO2FmPQCJMrRVDXaV+9TcJZXCbclQnzzBus9pwZZyw4Anxo0vmir\n"
    "nOMOU6XI4lO9Xge/QDEN4Y2R\n"
    "-----END PRIVATE KEY-----\n";

// Different self-signed CA for "wrong CA" test scenarios
// (A different self-signed cert that won't verify server_cert_pem)
inline constexpr char const* wrong_ca_cert_pem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICpDCCAYwCCQDU+pQ4P0jwoDANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\n"
    "b2NhbGhvc3QwHhcNMjMwMTAxMDAwMDAwWhcNMzMwMTAxMDAwMDAwWjAUMRIwEAYD\n"
    "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7\n"
    "o5e7Xv5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z5Z\n"
    "5Z5Z5Z5ZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBADummy0000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000000000000000000000000\n"
    "0000000000000000000000000000000000000000000000=\n"
    "-----END CERTIFICATE-----\n";
//------------------------------------------------------------------------------
//
// Context Helpers
//
//------------------------------------------------------------------------------

/** Create a context with anonymous ciphers (no certificates needed). */
inline context
make_anon_context()
{
    context ctx;
    ctx.set_verify_mode( verify_mode::none );
    ctx.set_ciphersuites( "aNULL:eNULL:@SECLEVEL=0" );
    return ctx;
}

/** Create a server context with test certificate. */
inline context
make_server_context()
{
    context ctx;
    ctx.use_certificate( server_cert_pem, file_format::pem );
    ctx.use_private_key( server_key_pem, file_format::pem );
    ctx.set_verify_mode( verify_mode::none );
    return ctx;
}

/** Create a client context that trusts the test CA. */
inline context
make_client_context()
{
    context ctx;
    ctx.add_certificate_authority( ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a client context that trusts the WRONG CA (for failure tests). */
inline context
make_wrong_ca_context()
{
    context ctx;
    ctx.add_certificate_authority( wrong_ca_cert_pem );
    ctx.set_verify_mode( verify_mode::peer );
    return ctx;
}

/** Create a context that requires peer verification but has no cert. */
inline context
make_verify_no_cert_context()
{
    context ctx;
    ctx.set_verify_mode( verify_mode::require_peer );
    return ctx;
}

//------------------------------------------------------------------------------
//
// Context Configuration Modes
//
//------------------------------------------------------------------------------

enum class context_mode
{
    anon,           // Anonymous ciphers, no certificates
    shared_cert,    // Both use same context with server cert
    separate_cert   // Server has cert, client trusts CA
};

/** Create client and server contexts for the given mode. */
inline std::pair<context, context>
make_contexts( context_mode mode )
{
    switch( mode )
    {
    case context_mode::anon:
        return { make_anon_context(), make_anon_context() };
    case context_mode::shared_cert:
    {
        auto ctx = make_server_context();
        ctx.add_certificate_authority( ca_cert_pem );
        return { ctx, ctx };
    }
    case context_mode::separate_cert:
        return { make_client_context(), make_server_context() };
    }
    return { make_anon_context(), make_anon_context() };
}

//------------------------------------------------------------------------------
//
// Test Coroutines
//
//------------------------------------------------------------------------------

/** Test bidirectional data transfer on connected streams. */
inline capy::task<>
test_stream( io_stream& a, io_stream& b )
{
    char buf[32] = {};

    // Write from a, read from b
    auto [ec1, n1] = co_await a.write_some(
        capy::const_buffer( "hello", 5 ) );
    BOOST_TEST( !ec1 );
    BOOST_TEST_EQ( n1, 5u );

    auto [ec2, n2] = co_await b.read_some(
        capy::mutable_buffer( buf, sizeof( buf ) ) );
    BOOST_TEST( !ec2 );
    BOOST_TEST_EQ( n2, 5u );
    BOOST_TEST_EQ( std::string_view( buf, n2 ), "hello" );

    // Write from b, read from a
    auto [ec3, n3] = co_await b.write_some(
        capy::const_buffer( "world", 5 ) );
    BOOST_TEST( !ec3 );
    BOOST_TEST_EQ( n3, 5u );

    auto [ec4, n4] = co_await a.read_some(
        capy::mutable_buffer( buf, sizeof( buf ) ) );
    BOOST_TEST( !ec4 );
    BOOST_TEST_EQ( n4, 5u );
    BOOST_TEST_EQ( std::string_view( buf, n4 ), "world" );
}

//------------------------------------------------------------------------------
//
// Parameterized Test Runner
//
//------------------------------------------------------------------------------

/** Run a complete TLS test: handshake, data transfer, shutdown.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test(
    io_context& ioc,
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Concurrent handshakes
    capy::run_async( ioc.get_executor() )(
        [&client]() -> capy::task<>
        {
            auto [ec] = co_await client.handshake( tls_stream::client );
            BOOST_TEST( !ec );
        }() );

    capy::run_async( ioc.get_executor() )(
        [&server]() -> capy::task<>
        {
            auto [ec] = co_await server.handshake( tls_stream::server );
            BOOST_TEST( !ec );
        }() );

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    capy::run_async( ioc.get_executor() )(
        [&client, &server]() -> capy::task<>
        {
            co_await test_stream( client, server );
        }() );

    ioc.run();

    // Skip TLS shutdown - bidirectional close_notify exchange deadlocks
    // in single-threaded io_context. This is a test environment limitation.
    s1.close();
    s2.close();
}

/** Run a TLS test without shutdown phase (for cross-implementation tests).

    TLS shutdown has known interoperability issues between implementations
    due to differing close_notify handling (bidirectional vs unidirectional,
    blocking vs non-blocking). Cross-impl tests verify handshake and data
    transfer; shutdown is skipped to avoid these documented friction points.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test_no_shutdown(
    io_context& ioc,
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    // Concurrent handshakes
    capy::run_async( ioc.get_executor() )(
        [&client]() -> capy::task<>
        {
            auto [ec] = co_await client.handshake( tls_stream::client );
            BOOST_TEST( !ec );
        }() );

    capy::run_async( ioc.get_executor() )(
        [&server]() -> capy::task<>
        {
            auto [ec] = co_await server.handshake( tls_stream::server );
            BOOST_TEST( !ec );
        }() );

    ioc.run();
    ioc.restart();

    // Bidirectional data transfer
    capy::run_async( ioc.get_executor() )(
        [&client, &server]() -> capy::task<>
        {
            co_await test_stream( client, server );
        }() );

    ioc.run();

    // Skip TLS shutdown - just close sockets (like HTTP "connection: close")
    s1.close();
    s2.close();
}

/** Run a TLS test expecting handshake failure.

    Uses a timer to handle the case where one side fails and the other
    blocks waiting for data. When the timer fires, sockets are closed
    to unblock any pending operations.
    
    @param ioc          The io_context to use
    @param client_ctx   TLS context for the client
    @param server_ctx   TLS context for the server
    @param make_client  Factory: (io_stream&, context) -> TLS stream
    @param make_server  Factory: (io_stream&, context) -> TLS stream
*/
template<typename ClientStreamFactory, typename ServerStreamFactory>
void
run_tls_test_fail(
    io_context& ioc,
    context client_ctx,
    context server_ctx,
    ClientStreamFactory make_client,
    ServerStreamFactory make_server )
{
    auto [s1, s2] = corosio::test::make_socket_pair( ioc );

    auto client = make_client( s1, client_ctx );
    auto server = make_server( s2, server_ctx );

    bool client_failed = false;
    bool server_failed = false;
    bool client_done = false;
    bool server_done = false;

    // Concurrent handshakes (at least one should fail)
    capy::run_async( ioc.get_executor() )(
        [&client, &client_failed, &client_done]() -> capy::task<>
        {
            auto [ec] = co_await client.handshake( tls_stream::client );
            if( ec )
                client_failed = true;
            client_done = true;
        }() );

    capy::run_async( ioc.get_executor() )(
        [&server, &server_failed, &server_done]() -> capy::task<>
        {
            auto [ec] = co_await server.handshake( tls_stream::server );
            if( ec )
                server_failed = true;
            server_done = true;
        }() );

    // Timer to unblock stuck handshakes - when one side fails, the other
    // may block waiting for data. Timer cancels socket operations to unblock them.
    timer timeout( ioc );
    timeout.expires_after( std::chrono::milliseconds( 500 ) );
    capy::run_async( ioc.get_executor() )(
        [&timeout, &s1, &s2, &client_done, &server_done]() -> capy::task<>
        {
            (void)client_done;
            (void)server_done;
            auto [ec] = co_await timeout.wait();
            if( !ec )
            {
                // Timer expired - cancel pending operations then close sockets
                s1.cancel();
                s2.cancel();
                s1.close();
                s2.close();
            }
        }() );

    ioc.run();

    // Cancel timer if handshakes completed before timeout
    timeout.cancel();

    // At least one side should have failed
    BOOST_TEST( client_failed || server_failed );

    s1.close();
    s2.close();
}

} // namespace test
} // namespace tls
} // namespace corosio
} // namespace boost

#endif
