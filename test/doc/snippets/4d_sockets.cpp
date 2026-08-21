//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Compiled fragments shown in pages/4.guide/4d.sockets.adoc.

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
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/buffers.hpp>

namespace corosio = boost::corosio;
namespace capy = boost::capy;
// end::assume[]

// The header guard makes the copy shown inside the range-connect
// fragment expand to nothing; indented to match the second region so
// the page's indent=0 renders both flush.
    // tag::connect_range[]
    #include <boost/corosio/connect.hpp>
    // end::connect_range[]

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/resolver.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "test_suite.hpp"

namespace {

// Fragments that connect to a remote peer are compiled but never
// executed; loopback fragments run over test::make_socket_pair.

[[maybe_unused]] capy::task<>
overview_fragment(corosio::io_context& ioc)
{
    // tag::overview[]
    corosio::tcp_socket s(ioc);

    // connect() opens the socket automatically
    auto [ec] = co_await s.connect(
        corosio::endpoint(corosio::ipv4_address::loopback(), 8080));

    char buf[1024];
    auto [read_ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    // end::overview[]
}

void
construct_fragment(corosio::io_context& ioc)
{
    // tag::construct[]
    // From io_context
    corosio::tcp_socket s1(ioc);

    // From executor
    auto ex = ioc.get_executor();
    corosio::tcp_socket s2(ex);
    // end::construct[]
}

void
open_fragment(corosio::tcp_socket& s)
{
    // tag::open[]
    // Creates an IPv4 TCP socket and associates it with the platform
    // reactor (IOCP on Windows, epoll/kqueue/select on POSIX)
    if (auto ec = s.open())
        return;  // report the error
    // end::open[]
}

void
close_fragment(corosio::tcp_socket& s)
{
    // tag::close[]
    s.close();  // Cancels pending ops, closes socket
    // end::close[]
}

void
is_open_fragment(corosio::tcp_socket& s)
{
    // tag::is_open[]
    if (s.is_open())
    {
        // Socket ready for I/O
    }
    // end::is_open[]
}

[[maybe_unused]] capy::task<>
connect_fragment(corosio::tcp_socket& s, corosio::endpoint endpoint)
{
    // tag::connect[]
    auto [ec] = co_await s.connect(endpoint);
    // end::connect[]
}

[[maybe_unused]] capy::task<>
connect_throw_fragment(corosio::tcp_socket& s, corosio::endpoint endpoint)
{
    // tag::connect_throw[]
    if (auto [ec] = co_await s.connect(endpoint); ec)
        throw std::system_error(ec);  // Throws on error
    // end::connect_throw[]
}

[[maybe_unused]] capy::task<>
connect_range_fragment(corosio::io_context& ioc)
{
    // tag::connect_range[]

    corosio::resolver r(ioc);
    auto [rec, results] = co_await r.resolve("www.boost.org", "80");
    if (rec)
        co_return;

    corosio::tcp_socket s(ioc);
    auto [cec, ep] = co_await corosio::connect(s, results);
    if (cec)
        co_return;
    // `ep` is the endpoint that accepted the connection.
    // end::connect_range[]
}

// The page displays the library's signature; declaring it here keeps
// the shown declaration honest against the real one.
// tag::connect_signature[]
template<class Socket, std::ranges::input_range Range>
    requires std::convertible_to<
        std::ranges::range_reference_t<Range>,
        typename Socket::endpoint_type>
capy::task<capy::io_result<typename Socket::endpoint_type>>
connect(Socket& s, Range endpoints);
// end::connect_signature[]

[[maybe_unused]] capy::task<>
connect_condition_fragment(
    corosio::tcp_socket& s, corosio::resolver_results results)
{
    // tag::connect_condition[]
    auto [ec, ep] = co_await corosio::connect(
        s,
        results,
        [](std::error_code const&, corosio::endpoint const& e) {
            return e.is_v4();  // IPv4 only.
        });
    // end::connect_condition[]
}

[[maybe_unused]] capy::task<>
connect_iterator_fragment(
    corosio::tcp_socket& s, std::vector<corosio::endpoint> const& v)
{
    // tag::connect_iterator[]
    auto [ec, it] = co_await corosio::connect(s, v.begin(), v.end());
    if (!ec)
        std::cout << "connected to index " << (it - v.begin()) << "\n";
    // end::connect_iterator[]
}

capy::task<>
read_some_fragment(corosio::tcp_socket& s, std::size_t& bytes_read)
{
    // tag::read_some[]
    char buf[1024];
    auto [ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));
    // end::read_some[]
    if (!ec)
        bytes_read = n;
}

capy::task<>
read_eof_fragment(
    corosio::tcp_socket& s, capy::mutable_buffer buf, bool& got_eof)
{
    // tag::read_eof[]
    auto [ec, n] = co_await s.read_some(buf);

    if (ec == capy::cond::eof)
    {
        // Connection closed normally
    }
    // end::read_eof[]
    got_eof = (ec == capy::cond::eof);
}

capy::task<>
read_all_fragment(
    corosio::tcp_socket& s, capy::mutable_buffer buf, std::size_t& total)
{
    // tag::read_all[]
    auto [ec, n] = co_await capy::read(s, buf);
    // n == buffer_size(buf) or error occurred
    // end::read_all[]
    if (!ec)
        total = n;
}

capy::task<>
write_some_fragment(corosio::tcp_socket& s, std::size_t& bytes_written)
{
    // tag::write_some[]
    std::string msg = "Hello";
    auto [ec, n] = co_await s.write_some(
        capy::const_buffer(msg.data(), msg.size()));
    // end::write_some[]
    if (!ec)
        bytes_written = n;
}

capy::task<>
write_all_fragment(
    corosio::tcp_socket& s, capy::const_buffer buf, std::size_t& total)
{
    // tag::write_all[]
    auto [ec, n] = co_await capy::write(s, buf);
    // n == buffer_size(buf) or error occurred
    // end::write_all[]
    if (!ec)
        total = n;
}

void
cancel_fragment(corosio::tcp_socket& s)
{
    // tag::cancel[]
    s.cancel();
    // end::cancel[]
}

// Cancellation delivery needs a stop source driving the launch; the
// fragment only shows that the awaiting code does not change.
[[maybe_unused]] capy::task<>
stop_token_fragment(corosio::tcp_socket& s, capy::mutable_buffer buf)
{
    // tag::stop_token[]
    // Inside a coroutine launched with a stop token:
    auto [ec, n] = co_await s.read_some(buf);
    // Automatically cancelled if stop is requested
    // end::stop_token[]
}

void
move_assign_fragment(corosio::tcp_socket& s1, corosio::tcp_socket& s2)
{
    // tag::move_assign[]
    s1 = std::move(s2);  // Closes s1's socket if open, then moves s2
    // end::move_assign[]
}

capy::const_buffer some_buffer("hi", 2);

// Indented to match the second region inside the caller below, so the
// page's indent=0 renders both flush.
    // tag::io_stream_poly[]
    capy::task<void> send_data(corosio::io_stream& stream)
    {
        std::ignore = co_await capy::write(stream, some_buffer);
    }
    // end::io_stream_poly[]

// The shown caller writes on a socket that was never connected, so it
// is compiled but never executed; the test runs send_data over a
// connected loopback socket instead.
[[maybe_unused]] capy::task<>
io_stream_poly_fragment(corosio::io_context& ioc)
{
    // tag::io_stream_poly[]

    // Works with socket, wolfssl_stream, or any io_stream
    corosio::tcp_socket sock(ioc);
    co_await send_data(sock);
    // end::io_stream_poly[]
}

capy::task<>
buffer_sequences_fragment(corosio::tcp_socket& s)
{
    char data[8];
    std::size_t size = sizeof(data);
    char header[4];
    std::size_t header_size = sizeof(header);
    char body[4];
    std::size_t body_size = sizeof(body);

    // tag::buffer_sequences[]
    // Single buffer
    capy::mutable_buffer buf(data, size);
    auto [ec, n] = co_await s.read_some(buf);

    // Multiple buffers (scatter/gather I/O)
    std::array<capy::mutable_buffer, 2> bufs = {
        capy::mutable_buffer(header, header_size),
        capy::mutable_buffer(body, body_size)
    };
    std::tie(ec, n) = co_await s.read_some(bufs);
    // end::buffer_sequences[]
}

// tag::echo_client[]
capy::task<void> echo_client(corosio::io_context& ioc)
{
    corosio::tcp_socket s(ioc);

    if (auto [ec] = co_await s.connect(
            corosio::endpoint(corosio::ipv4_address::loopback(), 8080)); ec)
        throw std::system_error(ec);

    std::string msg = "Hello, server!";
    if (auto [ec, n] = co_await capy::write(
            s, capy::const_buffer(msg.data(), msg.size())); ec)
        throw std::system_error(ec);

    char buf[1024];
    auto [ec, n] = co_await s.read_some(
        capy::mutable_buffer(buf, sizeof(buf)));

    if (!ec)
        std::cout << "Server replied: "
                  << std::string_view(buf, n) << "\n";
}
// end::echo_client[]

capy::task<>
peer_write(corosio::tcp_socket& s, std::string_view text)
{
    co_await capy::write(s, capy::const_buffer(text.data(), text.size()));
}

capy::task<>
peer_close(corosio::tcp_socket& s)
{
    s.close();  // graceful FIN
    co_return;
}

struct sockets_test
{
    void
    testConstruct()
    {
        corosio::io_context ioc;
        construct_fragment(ioc);
    }

    void
    testOpenClose()
    {
        corosio::io_context ioc;
        corosio::tcp_socket s(ioc);
        open_fragment(s);
        BOOST_TEST(s.is_open());
        is_open_fragment(s);
        close_fragment(s);
        BOOST_TEST(!s.is_open());
    }

    void
    testReadSome()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        std::size_t bytes_read = 0;
        capy::run_async(ex)(peer_write(b, "hello"));
        capy::run_async(ex)(read_some_fragment(a, bytes_read));
        ioc.run();

        BOOST_TEST(bytes_read > 0);
        a.close();
        b.close();
    }

    void
    testReadEof()
    {
        corosio::io_context ioc;
        // Linger=false => graceful FIN on close.
        auto [a, b] = corosio::test::make_socket_pair<
            corosio::tcp_socket, corosio::tcp_acceptor, false>(ioc);
        auto ex = ioc.get_executor();

        char storage[64];
        bool got_eof = false;
        capy::run_async(ex)(peer_close(b));
        capy::run_async(ex)(read_eof_fragment(
            a, capy::mutable_buffer(storage, sizeof(storage)), got_eof));
        ioc.run();

        BOOST_TEST(got_eof);
        a.close();
    }

    void
    testReadAll()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        char storage[5];
        std::size_t total = 0;
        capy::run_async(ex)(peer_write(b, "hello"));
        capy::run_async(ex)(read_all_fragment(
            a, capy::mutable_buffer(storage, sizeof(storage)), total));
        ioc.run();

        BOOST_TEST(total == sizeof(storage));
        a.close();
        b.close();
    }

    void
    testWriteSome()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        std::size_t bytes_written = 0;
        capy::run_async(ex)(write_some_fragment(a, bytes_written));
        ioc.run();

        BOOST_TEST(bytes_written == 5);
        a.close();
        b.close();
    }

    void
    testWriteAll()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        std::string_view payload = "payload";
        std::size_t total = 0;
        capy::run_async(ex)(write_all_fragment(
            a, capy::const_buffer(payload.data(), payload.size()), total));
        ioc.run();

        BOOST_TEST(total == payload.size());
        a.close();
        b.close();
    }

    void
    testCancel()
    {
        corosio::io_context ioc;
        corosio::tcp_socket s(ioc);
        BOOST_TEST(!s.open());
        cancel_fragment(s);
        s.close();
    }

    void
    testMoveAssign()
    {
        corosio::io_context ioc;
        corosio::tcp_socket s1(ioc);
        corosio::tcp_socket s2(ioc);
        BOOST_TEST(!s2.open());
        move_assign_fragment(s1, s2);
        BOOST_TEST(s1.is_open());
        s1.close();
    }

    void
    testSendData()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        capy::run_async(ex)(send_data(a));
        ioc.run();

        a.close();
        b.close();
    }

    void
    testBufferSequences()
    {
        corosio::io_context ioc;
        auto [a, b] = corosio::test::make_socket_pair(ioc);
        auto ex = ioc.get_executor();

        // 16 bytes cover both reads: the first takes at most 8, so
        // data remains for the scatter read regardless of the split.
        capy::run_async(ex)(peer_write(b, "0123456789abcdef"));
        capy::run_async(ex)(buffer_sequences_fragment(a));
        ioc.run();

        a.close();
        b.close();
    }

    void
    run()
    {
        testConstruct();
        testOpenClose();
        testReadSome();
        testReadEof();
        testReadAll();
        testWriteSome();
        testWriteAll();
        testCancel();
        testMoveAssign();
        testSendData();
        testBufferSequences();
    }
};

} // namespace

TEST_SUITE(sockets_test, "boost.corosio.doc.4d_sockets");
