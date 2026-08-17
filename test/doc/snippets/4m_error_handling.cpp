//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4m.error-handling.adoc.

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
#include <boost/corosio.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <iostream>
#include <system_error>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
using namespace std::chrono_literals;
// end::assume[]

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <cstddef>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "test_suite.hpp"

namespace {

// A port that was just bound and released: connecting to it on
// loopback yields a real connection_refused.
corosio::endpoint
closed_endpoint(corosio::io_context& ioc)
{
    corosio::tcp_acceptor acc(ioc);
    acc.open();
    acc.set_option(corosio::socket_option::reuse_address(true));
    if (auto ec = acc.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0)))
        throw std::runtime_error("bind failed: " + ec.message());
    auto ep = acc.local_endpoint();
    acc.close();
    return ep;
}

// Produce a real OS error code for the category/comparison fragments.
std::error_code
refused_connect_ec(corosio::io_context& ioc)
{
    std::error_code out;
    corosio::tcp_socket sock(ioc);
    sock.open();
    capy::run_async(ioc.get_executor())(
        [](corosio::tcp_socket& s, corosio::endpoint ep,
           std::error_code& o) -> capy::task<> {
            auto [ec] = co_await s.connect(ep);
            o = ec;
        }(sock, closed_endpoint(ioc), out));
    ioc.run();
    ioc.restart();
    return out;
}

capy::task<>
bindings_void_result(
    corosio::tcp_socket& sock,
    corosio::endpoint endpoint,
    std::error_code& out)
{
    // tag::structured_bindings[]
    // Void result
    auto [ec] = co_await sock.connect(endpoint);
    if (ec)
        std::cerr << "Connect failed: " << ec.message() << "\n";

    // end::structured_bindings[]
    out = ec;
}

capy::task<>
bindings_value_result(
    corosio::tcp_socket& sock,
    capy::mutable_buffer buffer,
    std::error_code& out_ec,
    std::size_t& out_n)
{
    // tag::structured_bindings[]
    // Value result
    auto [ec, n] = co_await sock.read_some(buffer);
    if (ec)
        std::cerr << "Read failed: " << ec.message() << "\n";
    else
        std::cout << "Read " << n << " bytes\n";
    // end::structured_bindings[]
    out_ec = ec;
    out_n = n;
}

capy::task<>
direct_members(
    corosio::tcp_socket& sock,
    corosio::endpoint endpoint,
    std::error_code& out)
{
    // tag::direct_members[]
    auto result = co_await sock.connect(endpoint);
    if (!std::get<0>(result))
        std::cout << "Connected successfully\n";
    else
        std::cerr << "Failed: " << std::get<0>(result).message() << "\n";
    // end::direct_members[]
    out = std::get<0>(result);
}

capy::task<>
throw_explicit(
    corosio::tcp_socket& sock,
    capy::mutable_buffer buffer,
    std::size_t& out_n)
{
    // tag::throw_explicit[]
    auto [ec, n] = co_await sock.read_some(buffer);
    if (ec)
        throw std::system_error(ec);
    // 'n' bytes were read
    // end::throw_explicit[]
    out_n = n;
}

capy::task<>
inspect_eof(
    corosio::tcp_socket& sock,
    capy::mutable_buffer buf,
    std::error_code& out)
{
    // tag::inspect_eof[]
    auto [ec, n] = co_await sock.read_some(buf);
    if (ec == capy::cond::eof)
    {
        std::cout << "End of stream after " << n << " bytes\n";
        // Not an exceptional condition
    }
    // end::inspect_eof[]
    out = ec;
}

capy::task<>
throw_style(
    corosio::tcp_socket& sock,
    corosio::endpoint endpoint,
    capy::const_buffer request,
    capy::mutable_buffer buffer,
    std::size_t& out_n)
{
    // tag::throw_style[]
    auto throw_on_error = [](auto result) {
        if (std::get<0>(result))
            throw std::system_error(std::get<0>(result));
        return result;
    };

    throw_on_error(co_await sock.connect(endpoint));
    throw_on_error(co_await capy::write(sock, request));
    auto [ec, n] = throw_on_error(co_await capy::read(sock, buffer));
    // end::throw_style[]
    out_n = n;
}

// The delay is awaited with an already-requested stop token, so the
// fragment sees a genuinely cancelled operation.
capy::task<>
canceled_condition(std::error_code& out)
{
    auto [ec] = co_await corosio::delay(1ms);
    // tag::canceled_condition[]
    if (ec == capy::cond::canceled)
        std::cout << "Operation was cancelled\n";
    // end::canceled_condition[]
    out = ec;
}

capy::task<>
timeout_vs_cancel(
    corosio::tcp_socket& sock,
    corosio::endpoint ep,
    std::error_code& out)
{
    // tag::timeout_vs_cancel[]
    auto [ec] = co_await corosio::timeout(sock.connect(ep), 3s);
    if (ec == capy::cond::timeout)
        std::cout << "Deadline elapsed before connecting\n";
    else if (ec == capy::cond::canceled)
        std::cout << "Cancelled before the deadline\n";
    // end::timeout_vs_cancel[]
    out = ec;
}

capy::task<>
eof_expected(
    corosio::tcp_socket& stream,
    capy::mutable_buffer buffer,
    std::error_code& out_ec,
    std::size_t& out_n)
{
    // tag::eof_expected[]
    auto [ec, n] = co_await capy::read(stream, buffer);
    if (ec == capy::cond::eof)
    {
        std::cout << "Stream ended, read " << n << " bytes total\n";
        // This is often expected, not an error
    }
    else if (ec)
    {
        std::cerr << "Unexpected error: " << ec.message() << "\n";
    }
    // end::eof_expected[]
    out_ec = ec;
    out_n = n;
}

capy::task<>
eof_filtered(
    corosio::tcp_socket& stream,
    capy::mutable_buffer response,
    std::size_t& out_n)
{
    // tag::eof_filtered[]
    auto [ec, n] = co_await capy::read(stream, response);
    if (ec && ec != capy::cond::eof)
        throw std::system_error(ec);
    // EOF is expected when server closes connection
    // end::eof_filtered[]
    out_n = n;
}

capy::task<>
partial_success(
    corosio::tcp_socket& stream,
    capy::const_buffer large_buffer,
    std::error_code& out)
{
    using capy::buffer_size;
    // tag::partial_success[]
    auto [ec, n] = co_await capy::write(stream, large_buffer);
    if (ec)
    {
        std::cerr << "Error after writing " << n << " of "
                  << buffer_size(large_buffer) << " bytes\n";
        // Can potentially resume from here
    }
    // end::partial_success[]
    out = ec;
}

void
category_checks(std::error_code ec)
{
    // tag::error_categories[]
    if (ec.category() == std::system_category())
    {
        // Operating system error
    }

    if (ec.category() == std::generic_category())
    {
        // Portable POSIX-style error
    }
    // end::error_categories[]
}

void
compare_errors(std::error_code ec)
{
    // tag::compare_errors[]
    // Specific error (platform-dependent)
    if (ec == std::errc::connection_refused)
    {
        // ...
    }

    // Error condition (portable)
    if (ec == capy::cond::canceled)
    {
        // Matches any cancellation error
    }

    if (ec == capy::cond::eof)
    {
        // Matches end-of-stream
    }
    // end::compare_errors[]
}

// The page shows `safe_operation` with `sock` and `endpoint` in scope;
// the fixture provides them without changing the shown code.
struct exception_safety_fixture
{
    corosio::tcp_socket sock;
    corosio::endpoint endpoint;

    exception_safety_fixture(corosio::io_context& ioc, corosio::endpoint ep)
        : sock(ioc)
        , endpoint(ep)
    {
        sock.open();
    }

    // tag::exception_safety[]
    capy::task<void> safe_operation()
    {
        try
        {
            auto [ec] = co_await sock.connect(endpoint);
            if (ec)
                throw std::system_error(ec);
        }
        catch (std::system_error const& e)
        {
            std::cerr << "Connect failed: " << e.what() << "\n";
            // Exception handled here, doesn't propagate
        }
    }
    // end::exception_safety[]
};

// tag::robust_connect[]
capy::task<void> connect_with_retry(
    corosio::io_context& ioc,
    corosio::endpoint ep,
    int max_retries)
{
    corosio::tcp_socket sock(ioc);

    for (int attempt = 0; attempt < max_retries; ++attempt)
    {
        sock.open();
        auto [ec] = co_await sock.connect(ep);

        if (!ec)
            co_return;  // Success

        std::cerr << "Attempt " << (attempt + 1)
                  << " failed: " << ec.message() << "\n";

        sock.close();

        // Wait before retry (exponential backoff)
        auto [dec] = co_await corosio::delay(std::chrono::seconds(1 << attempt));
        if (dec == capy::cond::canceled)
            co_return;  // Cancellation aborts the retry loop
    }

    throw std::runtime_error("Failed to connect after retries");
}
// end::robust_connect[]

struct error_handling_test
{
    void
    testResultShapes()
    {
        using capy::io_result;
        using corosio::resolver_results;
        // tag::result_shapes[]
        // Void result (connect, handshake)
        io_result<> r1;                  // Contains: ec

        // Single value (read_some, write_some)
        io_result<std::size_t> r2;       // Contains: ec, n (bytes transferred)

        // Typed result (resolve)
        io_result<resolver_results> r3;  // Contains: ec, results
        // end::result_shapes[]
        BOOST_TEST(!std::get<0>(r1));
        BOOST_TEST(!std::get<0>(r2));
        BOOST_TEST(!std::get<0>(r3));
    }

    void
    testStructuredBindings()
    {
        corosio::io_context ioc;
        corosio::tcp_socket sock(ioc);
        sock.open();
        std::error_code connect_ec;
        capy::run_async(ioc.get_executor())(
            bindings_void_result(sock, closed_endpoint(ioc), connect_ec));
        ioc.run();
        ioc.restart();
        BOOST_TEST(connect_ec == std::errc::connection_refused);

        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char data[64];
        std::error_code read_ec;
        std::size_t read_n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(capy::const_buffer("hello", 5));
            }(b));
        capy::run_async(ioc.get_executor())(
            bindings_value_result(
                a, capy::mutable_buffer(data, sizeof(data)),
                read_ec, read_n));
        ioc.run();
        BOOST_TEST(!read_ec);
        BOOST_TEST(read_n == 5);
    }

    void
    testDirectMembers()
    {
        corosio::io_context ioc;
        corosio::tcp_socket sock(ioc);
        sock.open();
        std::error_code out;
        capy::run_async(ioc.get_executor())(
            direct_members(sock, closed_endpoint(ioc), out));
        ioc.run();
        BOOST_TEST(out == std::errc::connection_refused);
    }

    void
    testThrowExplicit()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char data[64];
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(capy::const_buffer("hello", 5));
            }(b));
        capy::run_async(ioc.get_executor())(
            throw_explicit(
                a, capy::mutable_buffer(data, sizeof(data)), n));
        ioc.run();
        BOOST_TEST(n == 5);
    }

    void
    testInspectEof()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        // A FIN from the peer surfaces as the eof condition.
        b.shutdown(corosio::shutdown_send);
        char data[64];
        std::error_code out;
        capy::run_async(ioc.get_executor())(
            inspect_eof(
                a, capy::mutable_buffer(data, sizeof(data)), out));
        ioc.run();
        BOOST_TEST(out == capy::cond::eof);
    }

    void
    testThrowStyle()
    {
        corosio::io_context ioc;
        corosio::tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(corosio::socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto ep = acc.local_endpoint();

        corosio::tcp_socket psock(ioc);
        corosio::tcp_socket sock(ioc);
        sock.open();
        std::string request_text = "GET /\r\n";
        char received[16] = {};
        std::size_t got = 0;
        bool peer_ok = false;

        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_acceptor& a, corosio::tcp_socket& p,
               std::size_t reqlen, bool& ok) -> capy::task<> {
                auto [aec] = co_await a.accept(p);
                if (aec)
                    co_return;
                std::vector<char> req(reqlen);
                auto [rec, rn] = co_await capy::read(
                    p, capy::mutable_buffer(req.data(), req.size()));
                if (rec || rn != reqlen)
                    co_return;
                char resp[16];
                for (auto& c : resp)
                    c = 'x';
                auto [wec, wn] = co_await capy::write(
                    p, capy::const_buffer(resp, sizeof(resp)));
                ok = !wec && wn == sizeof(resp);
            }(acc, psock, request_text.size(), peer_ok));
        capy::run_async(ioc.get_executor())(
            throw_style(
                sock, ep,
                capy::const_buffer(
                    request_text.data(), request_text.size()),
                capy::mutable_buffer(received, sizeof(received)),
                got));
        ioc.run();
        acc.close();
        BOOST_TEST(peer_ok);
        BOOST_TEST(got == sizeof(received));
    }

    void
    testCanceledCondition()
    {
        corosio::io_context ioc;
        std::stop_source source;
        source.request_stop();
        std::error_code out;
        capy::run_async(ioc.get_executor(), source.get_token())(
            canceled_condition(out));
        ioc.run();
        BOOST_TEST(out == capy::cond::canceled);
    }

    void
    testTimeoutVsCancel()
    {
        // The coroutine's own stop token is already requested, so the
        // race reports cancellation rather than a timeout.
        corosio::io_context ioc;
        corosio::tcp_socket sock(ioc);
        sock.open();
        std::stop_source source;
        source.request_stop();
        std::error_code out;
        capy::run_async(ioc.get_executor(), source.get_token())(
            timeout_vs_cancel(sock, closed_endpoint(ioc), out));
        ioc.run();
        BOOST_TEST(out == capy::cond::canceled);
    }

    void
    testEofExpected()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char data[64];
        std::error_code out_ec;
        std::size_t out_n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(capy::const_buffer("hello", 5));
                peer.shutdown(corosio::shutdown_send);
            }(b));
        capy::run_async(ioc.get_executor())(
            eof_expected(
                a, capy::mutable_buffer(data, sizeof(data)),
                out_ec, out_n));
        ioc.run();
        BOOST_TEST(out_ec == capy::cond::eof);
        BOOST_TEST(out_n == 5);
    }

    void
    testEofFiltered()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char data[64];
        std::size_t out_n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(capy::const_buffer("hello", 5));
                peer.shutdown(corosio::shutdown_send);
            }(b));
        capy::run_async(ioc.get_executor())(
            eof_filtered(
                a, capy::mutable_buffer(data, sizeof(data)), out_n));
        ioc.run();
        BOOST_TEST(out_n == 5);
    }

    void
    testPartialSuccess()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        // The pair sets linger(0): closing the peer sends a RST, so a
        // large write fails partway through with a real error.
        b.close();
        std::vector<char> big(4 << 20, 'x');
        std::error_code out;
        capy::run_async(ioc.get_executor())(
            partial_success(
                a, capy::const_buffer(big.data(), big.size()), out));
        ioc.run();
        BOOST_TEST(out);
    }

    void
    testCategories()
    {
        corosio::io_context ioc;
        auto ec = refused_connect_ec(ioc);
        category_checks(ec);
        BOOST_TEST(ec.category() == std::system_category() ||
            ec.category() == std::generic_category());
    }

    void
    testCompareErrors()
    {
        corosio::io_context ioc;
        auto ec = refused_connect_ec(ioc);
        compare_errors(ec);
        BOOST_TEST(ec == std::errc::connection_refused);
        BOOST_TEST(!(ec == capy::cond::canceled));
        BOOST_TEST(!(ec == capy::cond::eof));
    }

    void
    testExceptionSafety()
    {
        corosio::io_context ioc;
        exception_safety_fixture fx(ioc, closed_endpoint(ioc));
        bool done = false;
        capy::run_async(ioc.get_executor())(
            [](exception_safety_fixture& f, bool& d) -> capy::task<> {
                co_await f.safe_operation();
                d = true;
            }(fx, done));
        ioc.run();
        BOOST_TEST(done);
    }

    void
    testRobustConnect()
    {
        corosio::io_context ioc;
        corosio::tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(corosio::socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(
            corosio::endpoint(corosio::ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());
        auto ep = acc.local_endpoint();

        corosio::tcp_socket psock(ioc);
        bool accepted = false;
        bool done = false;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_acceptor& a, corosio::tcp_socket& p,
               bool& ok) -> capy::task<> {
                auto [ec] = co_await a.accept(p);
                ok = !ec;
            }(acc, psock, accepted));
        capy::run_async(ioc.get_executor())(
            [](corosio::io_context& ctx, corosio::endpoint e,
               bool& d) -> capy::task<> {
                // First attempt succeeds, so no backoff delay runs.
                co_await connect_with_retry(ctx, e, 3);
                d = true;
            }(ioc, ep, done));
        ioc.run();
        acc.close();
        BOOST_TEST(accepted);
        BOOST_TEST(done);
    }

    void
    run()
    {
        testResultShapes();
        testStructuredBindings();
        testDirectMembers();
        testThrowExplicit();
        testInspectEof();
        testThrowStyle();
        testCanceledCondition();
        testTimeoutVsCancel();
        testEofExpected();
        testEofFiltered();
        testPartialSuccess();
        testCategories();
        testCompareErrors();
        testExceptionSafety();
        testRobustConnect();
    }
};

} // namespace

TEST_SUITE(error_handling_test, "boost.corosio.doc.4m_error_handling");
