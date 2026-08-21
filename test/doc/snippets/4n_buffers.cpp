//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4n.buffers.adoc.

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
#include <boost/capy/buffers.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/capy/task.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

// tag::buffer_slice_include[]
#include <boost/capy/buffers/buffer_slice.hpp>

// end::buffer_slice_include[]
// tag::consuming_buffers_include[]
#include <boost/capy/buffers/consuming_buffers.hpp>

// end::consuming_buffers_include[]
// tag::buffer_param_include[]
#include <boost/corosio/detail/buffer_param.hpp>

// end::buffer_param_include[]

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "test_suite.hpp"

namespace {

// The page's sketch of the socket signatures; compiling the
// declarations is the test.
namespace concept_sketch {

// tag::sequence_concepts[]
// Readable buffers (for writing to sockets)
template<capy::ConstBufferSequence ConstBufferSequence>
auto write_some(ConstBufferSequence const& buffers);

// Writable buffers (for reading from sockets)
template<capy::MutableBufferSequence MutableBufferSequence>
auto read_some(MutableBufferSequence const& buffers);
// end::sequence_concepts[]

} // namespace concept_sketch

capy::task<>
single_buffer_read(
    corosio::tcp_socket& sock, char* data, std::size_t size)
{
    // tag::single_buffer[]
    capy::mutable_buffer buf(data, size);
    auto [ec, n] = co_await sock.read_some(buf);  // Works directly
    // end::single_buffer[]
}

capy::task<>
multi_buffer_read(
    corosio::tcp_socket& sock,
    char* header, std::size_t header_size,
    char* body, std::size_t body_size)
{
    // tag::multi_buffer[]
    // Array of buffers
    std::array<capy::mutable_buffer, 2> bufs = {
        capy::mutable_buffer(header, header_size),
        capy::mutable_buffer(body, body_size)
    };
    auto [ec, n] = co_await sock.read_some(bufs);

    // end::multi_buffer[]
}

capy::task<>
multi_buffer_write(
    corosio::tcp_socket& sock,
    std::string const& header,
    std::string const& body)
{
    // tag::multi_buffer[]
    // Vector of buffers
    std::vector<capy::const_buffer> send_bufs;
    send_bufs.push_back(capy::const_buffer(header.data(), header.size()));
    send_bufs.push_back(capy::const_buffer(body.data(), body.size()));
    auto [ec, n] = co_await sock.write_some(send_bufs);
    // end::multi_buffer[]
}

capy::task<>
slice_writes(
    corosio::tcp_socket& sock,
    std::array<capy::const_buffer, 2> const& bufs)
{
    // tag::buffer_slice[]
    // Send only the first 16 bytes of the sequence
    auto [ec, n] = co_await capy::write(
        sock, capy::buffer_slice(bufs, 0, 16));
    if (ec)
        co_return;

    // Everything after the first 16 bytes, as a value
    auto rest = capy::buffer_slice(bufs, 16);
    std::tie(ec, n) = co_await capy::write(sock, rest);
    // end::buffer_slice[]
    sock.shutdown(corosio::shutdown_send);
}

capy::task<>
consume_reads(
    corosio::tcp_socket& sock,
    std::array<capy::mutable_buffer, 2> const& bufs,
    std::size_t& out_n)
{
    // tag::consuming_buffers[]
    capy::consuming_buffers consuming(bufs);

    // After reading n bytes:
    auto [ec, n] = co_await sock.read_some(consuming.data());
    consuming.consume(n);  // Advance past the bytes read

    // consuming.data() now spans the remaining unread bytes
    // end::consuming_buffers[]
    out_n = n;
}

// tag::buffer_param[]
void accept_any_buffer(corosio::buffer_param buffers)
{
    capy::mutable_buffer temp[8];
    std::size_t n = buffers.copy_to(temp, 8);
    // Use temp[0..n-1]
}

// end::buffer_param[]

// tag::lifetime[]
// WRONG: buffer outlives string
capy::task<void> bad_example(corosio::tcp_socket& sock)
{
    capy::const_buffer buf;
    {
        std::string temp = "Hello";
        buf = capy::const_buffer(temp.data(), temp.size());
    }  // temp destroyed here!

    std::ignore = co_await sock.write_some(buf);  // Undefined behavior
}

// CORRECT: keep storage alive
capy::task<void> good_example(corosio::tcp_socket& sock)
{
    std::string msg = "Hello";
    std::ignore = co_await sock.write_some(
        capy::const_buffer(msg.data(), msg.size()));
}
// end::lifetime[]

// The buggy coroutine is compiled but never run.
[[maybe_unused]] auto* const lifetime_demo = &bad_example;

capy::task<>
scatter_read(corosio::tcp_socket& sock, std::size_t& out_n)
{
    // tag::scatter_read[]
    struct message_header
    {
        std::uint32_t type;
        std::uint32_t length;
    };

    message_header header;
    char body[1024];

    std::array<capy::mutable_buffer, 2> read_bufs = {
        capy::mutable_buffer(&header, sizeof(header)),
        capy::mutable_buffer(body, sizeof(body))
    };

    auto [ec, n] = co_await sock.read_some(read_bufs);
    // Data fills header first, then body
    // end::scatter_read[]
    out_n = n;
}

capy::task<>
gather_write(corosio::tcp_socket& sock, std::size_t& out_n)
{
    // tag::gather_write[]
    std::string header = "HTTP/1.1 200 OK\r\n\r\n";
    std::string body = "Hello, World!";

    std::array<capy::const_buffer, 2> write_bufs = {
        capy::const_buffer(header.data(), header.size()),
        capy::const_buffer(body.data(), body.size())
    };

    auto [ec, n] = co_await sock.write_some(write_bufs);
    // Sends header followed by body in a single operation
    // end::gather_write[]
    out_n = n;
    sock.shutdown(corosio::shutdown_send);
}

// tag::read_header[]
struct packet_header
{
    std::uint32_t magic;
    std::uint32_t length;
};

capy::task<packet_header> read_header(corosio::io_stream& stream)
{
    packet_header header;
    auto [ec, n] = co_await capy::read(
        stream, capy::mutable_buffer(&header, sizeof(header)));

    if (ec)
        throw std::system_error(ec);

    co_return header;
}
// end::read_header[]

struct buffers_test
{
    void
    testMutableBuffer()
    {
        // tag::mutable_buffer[]
        char data[1024];
        capy::mutable_buffer buf(data, sizeof(data));
        // end::mutable_buffer[]
        BOOST_TEST(buf.data() == data);
        BOOST_TEST(buf.size() == sizeof(data));
    }

    void
    testConstBuffer()
    {
        // tag::const_buffer[]
        std::string msg = "Hello";
        capy::const_buffer buf(msg.data(), msg.size());
        // end::const_buffer[]
        BOOST_TEST(buf.data() == msg.data());
        BOOST_TEST(buf.size() == 5);
    }

    void
    testFromRawArrays()
    {
        // tag::from_raw_arrays[]
        char data[1024];
        capy::mutable_buffer mbuf(data, sizeof(data));

        const char* str = "Hello";
        capy::const_buffer cbuf(str, 5);
        // end::from_raw_arrays[]
        BOOST_TEST(mbuf.size() == 1024);
        BOOST_TEST(cbuf.data() == str);
        BOOST_TEST(cbuf.size() == 5);
    }

    void
    testFromString()
    {
        // tag::from_string[]
        std::string s = "Hello, World!";

        // Writable (be careful with string invalidation)
        capy::mutable_buffer mbuf(s.data(), s.size());

        // Read-only
        capy::const_buffer cbuf(s.data(), s.size());
        // end::from_string[]
        BOOST_TEST(mbuf.data() == s.data());
        BOOST_TEST(mbuf.size() == 13);
        BOOST_TEST(cbuf.size() == 13);
    }

    void
    testFromVector()
    {
        // tag::from_vector[]
        std::vector<char> vec(1024);
        capy::mutable_buffer buf(vec.data(), vec.size());
        // end::from_vector[]
        BOOST_TEST(buf.data() == vec.data());
        BOOST_TEST(buf.size() == 1024);
    }

    void
    testSingleBuffer()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char data[64] = {};
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(capy::const_buffer("ping", 4));
            }(b));
        capy::run_async(ioc.get_executor())(
            single_buffer_read(a, data, sizeof(data)));
        ioc.run();
        BOOST_TEST(std::memcmp(data, "ping", 4) == 0);
    }

    void
    testMultipleBuffers()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);

        // Scatter read: 8 bytes split across a 2-byte header and the
        // body.
        char header[2] = {};
        char body[6] = {};
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(
                    capy::const_buffer("ABCDEFGH", 8));
            }(b));
        capy::run_async(ioc.get_executor())(
            multi_buffer_read(
                a, header, sizeof(header), body, sizeof(body)));
        ioc.run();
        ioc.restart();
        BOOST_TEST(std::memcmp(header, "AB", 2) == 0);
        BOOST_TEST(std::memcmp(body, "CDEFGH", 6) == 0);

        // Gather write in the other direction.
        std::string hdr = "HDR";
        std::string bdy = "BODY!";
        char got[8] = {};
        std::size_t got_n = 0;
        capy::run_async(ioc.get_executor())(
            multi_buffer_write(a, hdr, bdy));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer, char* out, std::size_t len,
               std::size_t& n_out) -> capy::task<> {
                auto [ec, n] = co_await capy::read(
                    peer, capy::mutable_buffer(out, len));
                n_out = n;
            }(b, got, sizeof(got), got_n));
        ioc.run();
        BOOST_TEST(got_n == 8);
        BOOST_TEST(std::memcmp(got, "HDRBODY!", 8) == 0);
    }

    void
    testBufferSize()
    {
        // tag::buffer_size[]
        char header[16];
        char body[1024];

        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(header, sizeof(header)),
            capy::mutable_buffer(body, sizeof(body))
        };
        std::size_t total = capy::buffer_size(bufs);  // 1040
        // end::buffer_size[]
        BOOST_TEST(total == 1040);
    }

    void
    testBufferSlice()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        std::string part1(16, 'x');
        std::string part2(16, 'y');
        std::array<capy::const_buffer, 2> bufs = {
            capy::const_buffer(part1.data(), part1.size()),
            capy::const_buffer(part2.data(), part2.size())
        };

        std::string got;
        bool eof = false;
        capy::run_async(ioc.get_executor())(slice_writes(a, bufs));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer, std::string& out,
               bool& eof_out) -> capy::task<> {
                char chunk[64];
                for (;;)
                {
                    auto [ec, n] = co_await peer.read_some(
                        capy::mutable_buffer(chunk, sizeof(chunk)));
                    out.append(chunk, n);
                    if (ec)
                    {
                        eof_out = ec == capy::cond::eof;
                        co_return;
                    }
                }
            }(b, got, eof));
        ioc.run();
        BOOST_TEST(eof);
        BOOST_TEST(got == part1 + part2);
    }

    void
    testConsumingBuffers()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char head[4] = {};
        char tail[8] = {};
        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(head, sizeof(head)),
            capy::mutable_buffer(tail, sizeof(tail))
        };
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                co_await peer.write_some(
                    capy::const_buffer("0123456789", 10));
            }(b));
        capy::run_async(ioc.get_executor())(consume_reads(a, bufs, n));
        ioc.run();
        BOOST_TEST(n > 0);
        BOOST_TEST(std::memcmp(head, "0123", 4) == 0);
    }

    void
    testBufferParam()
    {
        char header[16];
        char body[64];
        // tag::buffer_param_call[]
        // Works with any buffer sequence (implicit conversion)
        std::array<capy::mutable_buffer, 2> bufs = {
            capy::mutable_buffer(header, sizeof(header)),
            capy::mutable_buffer(body, sizeof(body))
        };
        accept_any_buffer(bufs);
        // end::buffer_param_call[]
        BOOST_TEST(capy::buffer_size(bufs) == 80);
    }

    void
    testLifetime()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        char got[8] = {};
        std::size_t got_n = 0;
        capy::run_async(ioc.get_executor())(good_example(a));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer, char* out, std::size_t len,
               std::size_t& n_out) -> capy::task<> {
                auto [ec, n] = co_await peer.read_some(
                    capy::mutable_buffer(out, len));
                n_out = n;
            }(b, got, sizeof(got), got_n));
        ioc.run();
        BOOST_TEST(got_n == 5);
        BOOST_TEST(std::memcmp(got, "Hello", 5) == 0);
    }

    void
    testStringInvalidation()
    {
        // tag::string_invalidation[]
        std::string s = "Hello";
        capy::mutable_buffer buf(s.data(), s.size());

        s += " World";  // May reallocate, invalidating buf!

        // Use buf here: UNDEFINED BEHAVIOR
        // end::string_invalidation[]
        BOOST_TEST(s == "Hello World");
    }

    void
    testScatterRead()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        std::size_t n = 0;
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer) -> capy::task<> {
                char msg[16] = {};
                co_await peer.write_some(
                    capy::const_buffer(msg, sizeof(msg)));
            }(b));
        capy::run_async(ioc.get_executor())(scatter_read(a, n));
        ioc.run();
        BOOST_TEST(n > 0);
    }

    void
    testGatherWrite()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        std::size_t written = 0;
        std::string got;
        capy::run_async(ioc.get_executor())(gather_write(a, written));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer,
               std::string& out) -> capy::task<> {
                char chunk[64];
                for (;;)
                {
                    auto [ec, n] = co_await peer.read_some(
                        capy::mutable_buffer(chunk, sizeof(chunk)));
                    out.append(chunk, n);
                    if (ec)
                        co_return;
                }
            }(b, got));
        ioc.run();
        BOOST_TEST(written == 32);
        BOOST_TEST(got == "HTTP/1.1 200 OK\r\n\r\nHello, World!");
    }

    void
    testReadHeader()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        packet_header sent{0xC0FFEE42, 1234};
        packet_header received{};
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& peer,
               packet_header h) -> capy::task<> {
                co_await peer.write_some(
                    capy::const_buffer(&h, sizeof(h)));
            }(b, sent));
        capy::run_async(ioc.get_executor())(
            [](corosio::tcp_socket& sock,
               packet_header& out) -> capy::task<> {
                out = co_await read_header(sock);
            }(a, received));
        ioc.run();
        BOOST_TEST(received.magic == sent.magic);
        BOOST_TEST(received.length == sent.length);
    }

    void
    run()
    {
        testMutableBuffer();
        testConstBuffer();
        testFromRawArrays();
        testFromString();
        testFromVector();
        testSingleBuffer();
        testMultipleBuffers();
        testBufferSize();
        testBufferSlice();
        testConsumingBuffers();
        testBufferParam();
        testLifetime();
        testStringInvalidation();
        testScatterRead();
        testGatherWrite();
        testReadHeader();
    }
};

} // namespace

TEST_SUITE(buffers_test, "boost.corosio.doc.4n_buffers");
