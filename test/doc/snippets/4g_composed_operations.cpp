//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4g.composed-operations.adoc.

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

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

// tag::assume[]
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/buffers.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

// tag::slice_helper[]
#include <boost/capy/buffers/buffer_slice.hpp>
// end::slice_helper[]

#include <boost/capy/detail/except.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <iostream>
#include <stop_token>
#include <string>
#include <system_error>

#include "test_suite.hpp"

namespace {

// The pages show the algorithms with function-style synopses; the real
// entities have identical call signatures. The declarations only need
// to compile.
namespace synopsis {

using namespace boost::capy;

// tag::read_signature[]
auto
read(
    ReadStream auto& stream,
    MutableBufferSequence auto buffers) ->
        capy::io_task<std::size_t>;
// end::read_signature[]

// tag::write_signature[]
auto
write(
    WriteStream auto& stream,
    ConstBufferSequence auto buffers) ->
        capy::io_task<std::size_t>;
// end::write_signature[]

// tag::slice_interface[]
template<class BufferSequence>
    requires MutableBufferSequence<BufferSequence>
          || ConstBufferSequence<BufferSequence>
slice_type<BufferSequence>
buffer_slice(
    BufferSequence const& seq,
    std::size_t offset = 0,
    std::size_t length = (std::numeric_limits<std::size_t>::max)());
// end::slice_interface[]

} // namespace synopsis

capy::task<> primitives_frag(corosio::tcp_socket& s)
{
    // tag::primitives[]
    char buf[1024];
    auto [ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    // n could be 1, 100, 500, or 1024 - no guarantee
    // end::primitives[]
    BOOST_TEST(!ec);
    BOOST_TEST(n > 0);
}

capy::task<> read_full_frag(corosio::tcp_socket& stream)
{
    // tag::read_full[]
    char buf[1024];
    auto [ec, n] = co_await capy::read(
        stream, capy::mutable_buffer(buf, sizeof(buf)));

    // Either:
    // - n == 1024 and ec is default (success)
    // - ec == capy::cond::eof and n < 1024 (reached end of stream)
    // - ec is some other error
    // end::read_full[]
    BOOST_TEST(!ec);
    BOOST_TEST_EQ(n, sizeof(buf));
}

capy::task<> read_until_eof_frag(
    corosio::tcp_socket& stream, std::string& out)
{
    // tag::read_until_eof[]
    std::string content;
    char chunk[2048];
    for (;;)
    {
        auto [ec, n] = co_await stream.read_some(
            capy::mutable_buffer(chunk, sizeof(chunk)));
        content.append(chunk, n);
        if (ec == capy::cond::eof)
            break;  // success: the whole stream was consumed
        if (ec)
            throw std::system_error(ec);
    }
    // end::read_until_eof[]
    out = content;
}

capy::task<> write_full_frag(corosio::tcp_socket& stream)
{
    // tag::write_full[]
    std::string msg = "Hello, World!";
    auto [ec, n] = co_await capy::write(
        stream, capy::const_buffer(msg.data(), msg.size()));

    // Either:
    // - n == msg.size() and ec is default (all data written)
    // - ec is an error
    // end::write_full[]
    BOOST_TEST(!ec);
    BOOST_TEST_EQ(n, msg.size());
}

capy::task<> error_bindings_frag(
    corosio::tcp_socket& stream, capy::mutable_buffer buf,
    std::error_code& out)
{
    // tag::error_bindings[]
    auto [ec, n] = co_await capy::read(stream, buf);
    if (ec)
    {
        if (ec == capy::cond::eof)
            std::cout << "End of stream, read " << n << " bytes\n";
        else
            std::cerr << "Error: " << ec.message() << "\n";
    }
    // end::error_bindings[]
    out = ec;
}

capy::task<> error_exceptions_frag(
    corosio::tcp_socket& stream, capy::mutable_buffer buf)
{
    // tag::error_exceptions[]
    // For write (EOF doesn't apply)
    auto [wec, n] = co_await capy::write(stream, buf);
    if (wec)
        capy::detail::throw_system_error(wec);

    // For read (need to handle EOF)
    auto [rec, rn] = co_await capy::read(stream, buf);
    if (rec && rec != capy::cond::eof)
        capy::detail::throw_system_error(rec);
    // end::error_exceptions[]
    BOOST_TEST_EQ(rn, buf.size());
}

capy::task<> cancellation_frag(
    corosio::tcp_socket& stream, std::error_code& out)
{
    char storage[4096];
    capy::mutable_buffer large_buffer(storage, sizeof(storage));
    // tag::cancellation[]
    auto [ec, n] = co_await capy::read(stream, large_buffer);
    if (ec == capy::cond::canceled)
        std::cout << "Cancelled after reading " << n << " bytes\n";
    // end::cancellation[]
    out = ec;
}

// Contrasts call shapes only; issuing these reads needs a peer that
// sends this much data, so the coroutine is never launched.
[[maybe_unused]] capy::task<> multiple_buffers_frag(
    corosio::tcp_socket& stream, char* header, char* body,
    capy::mutable_buffer buf1, capy::mutable_buffer buf2)
{
    // tag::multiple_buffers[]
    // Efficient: single system call per read_some()
    std::array<capy::mutable_buffer, 2> bufs = {
        capy::mutable_buffer(header, 16),
        capy::mutable_buffer(body, 1024)};
    co_await capy::read(stream, bufs);

    // Less efficient: may require more system calls
    co_await capy::read(stream, buf1);
    co_await capy::read(stream, buf2);
    // end::multiple_buffers[]
}

// tag::http_response[]
capy::task<std::string> read_http_response(corosio::io_stream& stream)
{
    std::string response;
    char chunk[2048];
    for (;;)
    {
        auto [ec, n] = co_await stream.read_some(
            capy::mutable_buffer(chunk, sizeof(chunk)));
        response.append(chunk, n);
        if (ec == capy::cond::eof)
            break;
        if (ec)
            throw std::system_error(ec);
    }
    co_return response;
}
// end::http_response[]

capy::task<> send_and_close(
    corosio::tcp_socket& s, std::string_view text)
{
    auto [ec, n] = co_await capy::write(
        s, capy::const_buffer(text.data(), text.size()));
    BOOST_TEST(!ec);
    BOOST_TEST(!s.shutdown(corosio::shutdown_send));
}

struct composed_operations_test
{
    void
    testPrimitives()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& a, corosio::tcp_socket& b)
                -> capy::task<>
            {
                std::string_view text = "hi";
                co_await capy::write(
                    b, capy::const_buffer(text.data(), text.size()));
                co_await primitives_frag(a);
            }(s1, s2));
        ioc.run();
    }

    void
    testReadFull()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& a, corosio::tcp_socket& b)
                -> capy::task<>
            {
                std::string big(1024, 'x');
                co_await capy::write(
                    b, capy::const_buffer(big.data(), big.size()));
                co_await read_full_frag(a);
            }(s1, s2));
        ioc.run();
    }

    void
    testReadUntilEof()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        std::string content;
        capy::run_async(ioc.get_executor())(
            send_and_close(s2, "the whole stream"));
        capy::run_async(ioc.get_executor())(
            read_until_eof_frag(s1, content));
        ioc.run();
        BOOST_TEST_EQ(content, "the whole stream");
    }

    void
    testWriteFull()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        capy::run_async(ioc.get_executor())(write_full_frag(s1));
        ioc.run();
    }

    void
    testBufferSlice()
    {
        char header[16];
        char body[1024];
        // tag::slice_helper[]

        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(header, 16),
            capy::mutable_buffer(body, 1024)
        };

        // After reading 20 bytes, slice off what was consumed:
        auto rest = capy::buffer_slice(bufs, 20);
        // rest covers: 4 bytes of header remaining + full body
        // end::slice_helper[]
        BOOST_TEST_EQ(capy::buffer_size(rest), 1020u);
    }

    void
    testErrorBindings()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        char storage[64];
        std::error_code ec;
        capy::run_async(ioc.get_executor())(send_and_close(s2, "hello"));
        capy::run_async(ioc.get_executor())(error_bindings_frag(
            s1, capy::mutable_buffer(storage, sizeof(storage)), ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::eof);
    }

    void
    testErrorExceptions()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        char storage[16] = {};
        capy::run_async(ioc.get_executor())(
            send_and_close(s2, std::string_view("0123456789abcdef", 16)));
        capy::run_async(ioc.get_executor())(error_exceptions_frag(
            s1, capy::mutable_buffer(storage, sizeof(storage))));
        ioc.run();
    }

    void
    testCancellation()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        std::error_code ec;
        std::stop_source source;
        source.request_stop();
        capy::run_async(ioc.get_executor(), source.get_token())(
            cancellation_frag(s1, ec));
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    void
    testHttpResponse()
    {
        corosio::io_context ioc;
        auto [s1, s2] = corosio::test::make_socket_pair(ioc);
        std::string response;
        capy::run_async(ioc.get_executor())(
            send_and_close(s2, "HTTP/1.1 200 OK\r\n\r\n"));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& s, std::string& out) -> capy::task<>
            {
                out = co_await read_http_response(s);
            }(s1, response));
        ioc.run();
        BOOST_TEST_EQ(response, "HTTP/1.1 200 OK\r\n\r\n");
    }

    void
    run()
    {
        testPrimitives();
        testReadFull();
        testReadUntilEof();
        testWriteFull();
        testBufferSlice();
        testErrorBindings();
        testErrorExceptions();
        testCancellation();
        testHttpResponse();
    }
};

} // namespace

TEST_SUITE(
    composed_operations_test,
    "boost.corosio.doc.4g_composed_operations");
