//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header is self-contained.
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <chrono>
#include <string_view>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <stdexcept>
#include <string>

#include <unistd.h>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

#if BOOST_COROSIO_POSIX
namespace {

std::string
make_temp_socket_path()
{
    char tmpl[] = "/tmp/corosio_wait_XXXXXX";
    if (!::mkdtemp(tmpl))
        throw std::runtime_error("mkdtemp failed");
    std::string path(tmpl);
    path += "/sock";
    return path;
}

void
cleanup_path(std::string const& path)
{
    ::unlink(path.c_str());
    auto dir = path.substr(0, path.rfind('/'));
    ::rmdir(dir.c_str());
}

} // namespace
#endif

template<auto Backend>
struct wait_test
{
    // wait_read completes when the peer sends data, no bytes consumed.
    void testWaitReadAndNoConsume()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        constexpr std::string_view payload = "hello";

        std::error_code wait_ec;
        bool wait_done = false;
        std::error_code read_ec;
        std::size_t bytes_read = 0;
        std::array<char, 32> buf{};

        auto reader = [&]() -> capy::task<> {
            auto [ec1] = co_await s1.wait(wait_type::read);
            wait_ec    = ec1;
            wait_done  = true;
            if (ec1)
                co_return;
            auto [ec2, n] = co_await s1.read_some(
                capy::mutable_buffer(buf.data(), buf.size()));
            read_ec    = ec2;
            bytes_read = n;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, payload.size());
    }

    // wait_type::write completes immediately on a connected socket.
    // Corosio matches asio's IOCP behavior: writability is always
    // treated as ready, the wait does not park on a edge transition.
    void testWaitWriteImmediate()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto writer = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

#if BOOST_COROSIO_POSIX
    // local_stream_socket wait_read fires when the peer writes.
    void testWaitOnLocalStream()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        auto path = make_temp_socket_path();

        local_stream_acceptor acc(ioc);
        acc.open();
        auto bec = acc.bind(local_endpoint(path));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        client.open();

        auto accept_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(server);
            (void)ec;
        };
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(local_endpoint(path));
            (void)ec;
        };
        capy::run_async(ex)(accept_task());
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();

        constexpr std::string_view payload = "hi";
        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await server.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await client.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(writer());
        ioc.run();

        cleanup_path(path);

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }
#endif // BOOST_COROSIO_POSIX

    // Cancellation via socket.cancel() yields operation_canceled.
    void testCancellation()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            s1.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Acceptor wait_read fires when a client connects; accept then succeeds.
    void testAcceptorWait()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);
        auto port = acc.local_endpoint().port();

        std::error_code wait_ec;
        bool wait_done = false;
        std::error_code accept_ec;
        tcp_socket peer(ioc);
        tcp_socket client(ioc);

        auto waiter = [&]() -> capy::task<> {
            auto [ec1] = co_await acc.wait(wait_type::read);
            wait_ec    = ec1;
            wait_done  = true;
            if (ec1)
                co_return;
            auto [ec2] = co_await acc.accept(peer);
            accept_ec  = ec2;
        };
        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
            (void)ec;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(peer.is_open());
    }

    // UDP socket wait_read completes when a datagram arrives.
    void testWaitOnUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv(ioc);
        recv.open(udp::v4());
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        udp_socket send(ioc);
        send.open(udp::v4());

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await recv.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char dg[1] = { 'X' };
            auto [ec, n] = co_await send.send_to(
                capy::const_buffer(dg, sizeof(dg)),
                endpoint(ipv4_address::loopback(), port));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Cancel a UDP wait_read while it's parked. On IOCP this exercises
    // the auxiliary WSAPoll reactor's cancel_wait path, where the op
    // has no overlapped I/O pending so CancelIoEx is a no-op and the
    // cancellation must be delivered through the reactor itself.
    void testUdpCancellation()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            sock.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void run()
    {
        testWaitReadAndNoConsume();
        testWaitWriteImmediate();
#if BOOST_COROSIO_POSIX
        testWaitOnLocalStream();
#endif
        testCancellation();
        testAcceptorWait();
        testWaitOnUdp();
        testUdpCancellation();
    }
};

COROSIO_BACKEND_TESTS(wait_test, "boost.corosio.wait")

} // namespace boost::corosio
