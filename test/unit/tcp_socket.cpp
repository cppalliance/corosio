//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/tcp_socket.hpp>

#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>

#include <boost/capy/buffers/string_dynamic_buffer.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/buffers/make_buffer.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stop_token>
#include <stdexcept>

#if BOOST_COROSIO_POSIX
#include <unistd.h> // getpid()
#else
#include <process.h> // _getpid()
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {
namespace {

} // namespace

// Verify tcp_socket satisfies stream concepts

static_assert(capy::ReadStream<tcp_socket>);
static_assert(capy::WriteStream<tcp_socket>);

// Socket-specific tests

template<auto Backend>
struct tcp_socket_test
{
    void testConstruction()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        // Socket should not be open initially
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        // Open the tcp_socket
        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        // Close it
        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testBind()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        // Bind to loopback with ephemeral port
        auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);

        // Local endpoint should reflect the bind
        auto local = sock.local_endpoint();
        BOOST_TEST(local.port() != 0);
        BOOST_TEST(local.is_v4());

        sock.close();
    }

    void testBindThenConnect()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        ec = acc.listen();
        BOOST_TEST(!ec);
        auto server_port = acc.local_endpoint().port();

        tcp_socket client(ioc);
        tcp_socket server(ioc);
        client.open();

        // Bind client to specific local address before connecting
        ec = client.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto bound_port = client.local_endpoint().port();
        BOOST_TEST(bound_port != 0);

        auto connect_task = [&]() -> capy::task<> {
            auto [conn_ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), server_port));
            BOOST_TEST(!conn_ec);
        };

        auto accept_task = [&]() -> capy::task<> {
            auto [acc_ec] = co_await acc.accept(server);
            BOOST_TEST(!acc_ec);
        };

        capy::run_async(ioc.get_executor())(connect_task());
        capy::run_async(ioc.get_executor())(accept_task());
        ioc.run();

        // Client's local port should be the one we bound to
        BOOST_TEST(client.local_endpoint().port() == bound_port);

        // Server sees our bound port as the remote
        BOOST_TEST(server.remote_endpoint().port() == bound_port);

        client.close();
        server.close();
        acc.close();
    }

    void testBindClosedSocketThrows()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        // Bind on a closed socket should throw
        bool caught = false;
        try
        {
            auto ec = sock.bind(endpoint(ipv4_address::loopback(), 0));
            (void)ec;
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testBindV6()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open(tcp::v6());

        auto ec = sock.bind(endpoint(ipv6_address::loopback(), 0));
        BOOST_TEST(!ec);

        auto local = sock.local_endpoint();
        BOOST_TEST(local.port() != 0);
        BOOST_TEST(local.is_v6());

        sock.close();
    }

    void testBindAddressInUse()
    {
        io_context ioc(Backend);

        // Bind first socket to a specific port
        tcp_socket sock1(ioc);
        sock1.open();
        auto ec = sock1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec);
        auto port = sock1.local_endpoint().port();

        // Second bind to same port should fail
        tcp_socket sock2(ioc);
        sock2.open();
        ec = sock2.bind(endpoint(ipv4_address::loopback(), port));
        BOOST_TEST(ec);

        sock1.close();
        sock2.close();
    }

    void testBindNonLocalAddress()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        auto ec = sock.bind(endpoint(ipv4_address("1.2.3.4"), 0));
        BOOST_TEST(ec);

        sock.close();
    }

    void testMoveConstruct()
    {
        io_context ioc(Backend);
        tcp_socket sock1(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);

        // Move construct
        tcp_socket sock2(std::move(sock1));
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        tcp_socket sock1(ioc);
        tcp_socket sock2(ioc);
        sock1.open();
        BOOST_TEST_EQ(sock1.is_open(), true);
        BOOST_TEST_EQ(sock2.is_open(), false);

        // Move assign
        sock2 = std::move(sock1);
        BOOST_TEST_EQ(sock1.is_open(), false);
        BOOST_TEST_EQ(sock2.is_open(), true);

        sock2.close();
    }

    // Basic Read/Write Operations

    void testReadSome()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            auto [ec1, n1] =
                co_await a.write_some(capy::const_buffer("hello", 5));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 5u);

            char buf[32] = {};
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n2), "hello");
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testWriteSome()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            char const* messages[] = {"abc", "defgh", "ijklmnop"};
            for (auto msg : messages)
            {
                std::size_t len = std::strlen(msg);
                auto [ec, n] =
                    co_await a.write_some(capy::const_buffer(msg, len));
                BOOST_TEST(!ec);
                BOOST_TEST_EQ(n, len);

                char buf[32]   = {};
                auto [ec2, n2] = co_await b.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                BOOST_TEST(!ec2);
                BOOST_TEST_EQ(std::string_view(buf, n2), msg);
            }
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testPartialRead()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write 5 bytes but try to read into 1024-byte buffer
            auto [ec1, n1] =
                co_await a.write_some(capy::const_buffer("test!", 5));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 5u);

            char buf[1024] = {};
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            // read_some returns what's available, not buffer size
            BOOST_TEST_EQ(n2, 5u);
            BOOST_TEST_EQ(std::string_view(buf, n2), "test!");
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testSequentialReadWrite()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            char buf[32] = {};

            // First exchange
            (void)co_await a.write_some(capy::const_buffer("one", 3));
            auto [ec1, n1] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "one");

            // Second exchange
            (void)co_await a.write_some(capy::const_buffer("two", 3));
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(std::string_view(buf, n2), "two");

            // Third exchange
            (void)co_await a.write_some(capy::const_buffer("three", 5));
            auto [ec3, n3] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(std::string_view(buf, n3), "three");
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testBidirectionalSimultaneous()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            char buf[32] = {};

            // Write from a, read from b
            auto [ec1, n1] =
                co_await a.write_some(capy::const_buffer("from_a", 6));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 6u);

            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(std::string_view(buf, n2), "from_a");

            // Write from b, read from a
            auto [ec3, n3] =
                co_await b.write_some(capy::const_buffer("from_b", 6));
            BOOST_TEST(!ec3);
            BOOST_TEST_EQ(n3, 6u);

            auto [ec4, n4] =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec4);
            BOOST_TEST_EQ(std::string_view(buf, n4), "from_b");

            // Interleaved: write a, write b, read b, read a
            (void)co_await a.write_some(capy::const_buffer("msg_a", 5));
            (void)co_await b.write_some(capy::const_buffer("msg_b", 5));

            auto [ec5, n5] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec5);
            BOOST_TEST_EQ(std::string_view(buf, n5), "msg_a");

            auto [ec6, n6] =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec6);
            BOOST_TEST_EQ(std::string_view(buf, n6), "msg_b");
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // Buffer Variations

    void testEmptyBuffer()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write with empty buffer
            auto [ec1, n1] =
                co_await a.write_some(capy::const_buffer(nullptr, 0));
            // Empty write should succeed with 0 bytes
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 0u);

            // Send actual data so read can complete
            (void)co_await a.write_some(capy::const_buffer("x", 1));

            // Read with empty buffer should return 0
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(nullptr, 0));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 0u);

            // Drain the actual data
            char buf[8];
            (void)co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testSmallBuffer()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Single byte writes
            for (char c = 'A'; c <= 'E'; ++c)
            {
                auto [ec1, n1] =
                    co_await a.write_some(capy::const_buffer(&c, 1));
                BOOST_TEST(!ec1);
                BOOST_TEST_EQ(n1, 1u);

                char buf = 0;
                auto [ec2, n2] =
                    co_await b.read_some(capy::mutable_buffer(&buf, 1));
                BOOST_TEST(!ec2);
                BOOST_TEST_EQ(n2, 1u);
                BOOST_TEST_EQ(buf, c);
            }
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testLargeBuffer()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // 64KB data - larger than typical TCP segment
            constexpr std::size_t size = std::size_t{64} * 1024;
            std::vector<char> send_data(size);
            for (std::size_t i = 0; i < size; ++i)
                send_data[i] = static_cast<char>(i & 0xFF);

            std::vector<char> recv_data(size);
            std::size_t total_sent = 0;
            std::size_t total_recv = 0;

            // Send all data (may take multiple write_some calls)
            while (total_sent < size)
            {
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(
                        send_data.data() + total_sent, size - total_sent));
                BOOST_TEST(!ec);
                total_sent += n;
            }

            // Receive all data (may take multiple read_some calls)
            while (total_recv < size)
            {
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(
                        recv_data.data() + total_recv, size - total_recv));
                BOOST_TEST(!ec);
                if (ec)
                    break;
                total_recv += n;
            }

            BOOST_TEST_EQ(total_sent, size);
            BOOST_TEST_EQ(total_recv, size);
            BOOST_TEST(send_data == recv_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // EOF and Closure Handling

    void testReadAfterPeerClose()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write data then close
            (void)co_await a.write_some(capy::const_buffer("final", 5));
            a.close();

            // Read the data
            char buf[32] = {};
            auto [ec1, n1] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "final");

            // Next read should get EOF
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(ec2 == capy::cond::eof);
            BOOST_TEST_EQ(n2, 0u);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testWriteAfterPeerClose()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Close the receiving end
            b.close();

            // Give OS time to process the close
            timer t(a.context());
            t.expires_after(std::chrono::milliseconds(50));
            (void)co_await t.wait();

            // Writing to closed peer should eventually fail.
            // We need to write enough data to fill the tcp_socket buffer and
            // trigger the error. macOS has larger buffers than Linux.
            std::error_code last_ec;
            std::array<char, 8192> buf{}; // Larger buffer per write
            for (int i = 0; i < 100; ++i) // More iterations
            {
                auto [ec, n] = co_await a.write_some(
                    capy::const_buffer(buf.data(), buf.size()));
                last_ec = ec;
                if (ec)
                    break;
            }
            // Should get an error (broken pipe or similar)
            BOOST_TEST(last_ec);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // Cancellation

    void testCancelRead()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [&](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Start a timer to cancel the read
            timer t(a.context());
            t.expires_after(std::chrono::milliseconds(50));

            // Launch read that will block (no data available)
            bool read_done = false;
            std::error_code read_ec;

            // Store lambda in variable to ensure it outlives the coroutine.
            // Lambda coroutines capture 'this' by reference, so the lambda
            // must remain alive while the coroutine is suspended.
            auto nested_coro = [&b, &read_done, &read_ec]() -> capy::task<> {
                char buf[32];
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                read_ec   = ec;
                read_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait for timer then cancel
            (void)co_await t.wait();
            b.cancel();

            // Wait for read to complete
            timer t2(a.context());
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(read_done);
            BOOST_TEST(read_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testCloseWhileReading()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [&](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            timer t(a.context());
            t.expires_after(std::chrono::milliseconds(50));

            bool read_done = false;
            std::error_code read_ec;

            // Store lambda in variable to ensure it outlives the coroutine.
            // Lambda coroutines capture 'this' by reference, so the lambda
            // must remain alive while the coroutine is suspended.
            auto nested_coro = [&b, &read_done, &read_ec]() -> capy::task<> {
                char buf[32];
                auto [ec, n] = co_await b.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                read_ec   = ec;
                read_done = true;
            };
            capy::run_async(ioc.get_executor())(nested_coro());

            // Wait then close the tcp_socket
            (void)co_await t.wait();
            b.close();

            timer t2(a.context());
            t2.expires_after(std::chrono::milliseconds(50));
            (void)co_await t2.wait();

            BOOST_TEST(read_done);
            // Close should cancel pending operations
            BOOST_TEST(read_ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testStopTokenCancellation()
    {
        // Verifies that std::stop_token properly cancels pending I/O.
        // On Linux/epoll, this requires the backend to actually unregister from
        // epoll and post the operation to the scheduler, not just set a flag.
        // Uses tcp_socket I/O for synchronization instead of timers.
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source stop_src;
        bool read_done    = false;
        bool failsafe_hit = false;
        std::error_code read_ec;

        // Reader task - signals ready then blocks waiting for data
        auto reader_task = [&]() -> capy::task<> {
            // Signal we're about to start the blocking read
            (void)co_await s2.write_some(capy::const_buffer("R", 1));

            // Now block waiting for data that will never come
            char buf[32];
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            read_ec   = ec;
            read_done = true;
        };

        // Canceller task - waits for reader to be ready, then requests stop
        auto canceller_task = [&]() -> capy::task<> {
            // Wait for reader's "ready" signal
            char buf[1];
            (void)co_await s1.read_some(capy::mutable_buffer(buf, 1));

            // Reader is now blocked on read - request stop
            stop_src.request_stop();
        };

        // Failsafe task - detects if stop_token cancellation didn't work
        auto failsafe_task = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(1000));
            auto [ec] = co_await t.wait();
            // Only trigger failsafe if reader hasn't completed yet.
            // If read_done is true, stop_token cancellation worked.
            if (!ec && !read_done)
            {
                // Failsafe triggered - stop_token cancellation didn't work!
                failsafe_hit = true;
                s2.cancel();
            }
        };

        // Launch all tasks
        capy::run_async(
            ioc.get_executor(), stop_src.get_token())(reader_task());
        capy::run_async(ioc.get_executor())(canceller_task());
        capy::run_async(ioc.get_executor())(failsafe_task());

        ioc.run();

        BOOST_TEST(read_done);
        BOOST_TEST(read_ec == capy::cond::canceled);

        // CRITICAL: The failsafe should NOT have been hit.
        // If it was hit, it means stop_token didn't actually cancel the I/O.
        BOOST_TEST(!failsafe_hit);

        s1.close();
        s2.close();
    }

    // Composed Operations

    void testReadFull()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write exactly 100 bytes
            std::string send_data(100, 'X');
            (void)co_await capy::write(
                a, capy::const_buffer(send_data.data(), send_data.size()));

            // Read exactly 100 bytes using corosio::read
            char buf[100] = {};
            auto [ec, n] =
                co_await capy::read(b, capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, 100u);
            BOOST_TEST_EQ(std::string_view(buf, n), send_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testWriteFull()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            std::string send_data(500, 'Y');
            auto [ec1, n1] = co_await capy::write(
                a, capy::const_buffer(send_data.data(), send_data.size()));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 500u);

            // Read it back
            std::string recv_data(500, 0);
            auto [ec2, n2] = co_await capy::read(
                b, capy::mutable_buffer(recv_data.data(), recv_data.size()));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 500u);
            BOOST_TEST_EQ(recv_data, send_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testReadString()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            std::string send_data = "Hello, this is a test message!";
            (void)co_await capy::write(a, capy::make_buffer(send_data));
            a.close();

            // Read into string until EOF using dynamic buffer
            std::string result;
            auto [ec, n] =
                co_await capy::read(b, capy::string_dynamic_buffer(&result));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(n, send_data.size());
            BOOST_TEST_EQ(result, send_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testReadPartialEOF()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Send 50 bytes but try to read 100
            std::string send_data(50, 'Z');
            (void)co_await capy::write(
                a, capy::const_buffer(send_data.data(), send_data.size()));
            a.close();

            char buf[100] = {};
            auto [ec, n] =
                co_await capy::read(b, capy::mutable_buffer(buf, sizeof(buf)));
            // Should get EOF after reading available data
            BOOST_TEST(ec == capy::error::eof);
            BOOST_TEST_EQ(n, 50u);
            BOOST_TEST_EQ(std::string_view(buf, n), send_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // Shutdown

    void testShutdownSend()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write data then shutdown send
            (void)co_await a.write_some(capy::const_buffer("hello", 5));
            // Note: unqualified shutdown_send (not tcp_socket::shutdown_send)
            // to work around a GCC 11 ICE in tsubst_copy when a class-template
            // using-enum enumerator is referenced inside a lambda-coroutine.
            a.shutdown(shutdown_send);

            // Read the data
            char buf[32] = {};
            auto [ec1, n1] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "hello");

            // Next read should get EOF
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(ec2 == capy::cond::eof);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testShutdownReceive()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Shutdown receive on b (unqualified; see GCC 11 note above).
            b.shutdown(shutdown_receive);

            // b can still send
            (void)co_await b.write_some(capy::const_buffer("from_b", 6));

            char buf[32] = {};
            auto [ec, n] =
                co_await a.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec);
            BOOST_TEST_EQ(std::string_view(buf, n), "from_b");
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    void testShutdownOnClosedSocket()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        // Shutdown on closed tcp_socket should not crash.
        // Unqualified enumerators; see GCC 11 note above.
        sock.shutdown(shutdown_send);
        sock.shutdown(shutdown_receive);
        sock.shutdown(shutdown_both);
    }

    void testShutdownBothSendDirection()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // Write data then shutdown both (unqualified; see GCC 11 note above).
            (void)co_await a.write_some(capy::const_buffer("goodbye", 7));
            a.shutdown(shutdown_both);

            // Peer should receive the data
            char buf[32] = {};
            auto [ec1, n1] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(std::string_view(buf, n1), "goodbye");

            // Next read should get EOF
            auto [ec2, n2] =
                co_await b.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(ec2 == capy::cond::eof);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // Socket Options

    void testNoDelay()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::no_delay(true));
        BOOST_TEST_EQ(sock.get_option<socket_option::no_delay>().value(), true);

        sock.set_option(socket_option::no_delay(false));
        BOOST_TEST_EQ(
            sock.get_option<socket_option::no_delay>().value(), false);

        sock.set_option(socket_option::no_delay(true));
        BOOST_TEST_EQ(sock.get_option<socket_option::no_delay>().value(), true);

        sock.close();
    }

    void testKeepAlive()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::keep_alive(true));
        BOOST_TEST_EQ(
            sock.get_option<socket_option::keep_alive>().value(), true);

        sock.set_option(socket_option::keep_alive(false));
        BOOST_TEST_EQ(
            sock.get_option<socket_option::keep_alive>().value(), false);

        sock.set_option(socket_option::keep_alive(true));
        BOOST_TEST_EQ(
            sock.get_option<socket_option::keep_alive>().value(), true);

        sock.close();
    }

    void testReceiveBufferSize()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        int initial_size =
            sock.get_option<socket_option::receive_buffer_size>().value();
        BOOST_TEST(initial_size > 0);

        sock.set_option(socket_option::receive_buffer_size(65536));
        int new_size =
            sock.get_option<socket_option::receive_buffer_size>().value();
        BOOST_TEST(new_size > 0);

        sock.close();
    }

    void testSendBufferSize()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        int initial_size =
            sock.get_option<socket_option::send_buffer_size>().value();
        BOOST_TEST(initial_size > 0);

        sock.set_option(socket_option::send_buffer_size(65536));
        int new_size =
            sock.get_option<socket_option::send_buffer_size>().value();
        BOOST_TEST(new_size > 0);

        sock.close();
    }

    void testLinger()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::linger(true, 5));
        auto opts = sock.get_option<socket_option::linger>();
        BOOST_TEST_EQ(opts.enabled(), true);
        BOOST_TEST_EQ(opts.timeout(), 5);

        sock.set_option(socket_option::linger(false, 0));
        opts = sock.get_option<socket_option::linger>();
        BOOST_TEST_EQ(opts.enabled(), false);

        sock.set_option(socket_option::linger(true, 10));
        opts = sock.get_option<socket_option::linger>();
        BOOST_TEST_EQ(opts.enabled(), true);
        BOOST_TEST_EQ(opts.timeout(), 10);

        sock.close();
    }

    void testLingerValidation()
    {
        // Removed: negative timeout validation was in the old
        // named set_linger() method. The generic set_option()
        // delegates directly to setsockopt which handles invalid
        // values via its own error reporting.
    }

    void testSocketOptionsOnConnectedSocket()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        s1.set_option(socket_option::no_delay(true));
        BOOST_TEST_EQ(s1.get_option<socket_option::no_delay>().value(), true);

        s2.set_option(socket_option::no_delay(true));
        BOOST_TEST_EQ(s2.get_option<socket_option::no_delay>().value(), true);

        s1.set_option(socket_option::keep_alive(true));
        BOOST_TEST_EQ(s1.get_option<socket_option::keep_alive>().value(), true);

        int recv_size =
            s1.get_option<socket_option::receive_buffer_size>().value();
        BOOST_TEST(recv_size > 0);

        int send_size =
            s1.get_option<socket_option::send_buffer_size>().value();
        BOOST_TEST(send_size > 0);

        s1.close();
        s2.close();
    }

    void testGenericSetGetOption()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::no_delay(true));
        BOOST_TEST(sock.get_option<socket_option::no_delay>().value());

        sock.set_option(socket_option::no_delay(false));
        BOOST_TEST(!sock.get_option<socket_option::no_delay>().value());

        sock.close();
    }

    void testGenericBufferOption()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::receive_buffer_size(32768));
        int sz = sock.get_option<socket_option::receive_buffer_size>().value();
        BOOST_TEST(sz >= 32768);

        sock.close();
    }

    void testGenericLingerOption()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        sock.set_option(socket_option::linger(true, 5));
        auto lg = sock.get_option<socket_option::linger>();
        BOOST_TEST(lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 5);

        sock.close();
    }

    void testOptionAssignmentOperators()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        // boolean assignment and negation
        socket_option::no_delay nd(false);
        BOOST_TEST(!nd);
        nd = true;
        BOOST_TEST(nd.value());
        sock.set_option(nd);
        BOOST_TEST(sock.get_option<socket_option::no_delay>().value());

        nd = false;
        BOOST_TEST(!nd);
        sock.set_option(nd);
        BOOST_TEST(!sock.get_option<socket_option::no_delay>().value());

        // integer assignment
        socket_option::receive_buffer_size rbs(0);
        rbs = 32768;
        BOOST_TEST_EQ(rbs.value(), 32768);
        sock.set_option(rbs);
        BOOST_TEST(
            sock.get_option<socket_option::receive_buffer_size>().value() >=
            32768);

        // linger setters
        socket_option::linger lg;
        BOOST_TEST(!lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 0);
        lg.enabled(true);
        lg.timeout(3);
        BOOST_TEST(lg.enabled());
        BOOST_TEST_EQ(lg.timeout(), 3);
        sock.set_option(lg);
        auto lg2 = sock.get_option<socket_option::linger>();
        BOOST_TEST(lg2.enabled());
        BOOST_TEST_EQ(lg2.timeout(), 3);

        sock.close();
    }

    // Data Integrity

    void testLargeTransfer()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // 128KB payload
        constexpr std::size_t size = std::size_t{128} * 1024;
        std::vector<char> send_data(size);
        for (std::size_t i = 0; i < size; ++i)
            send_data[i] = static_cast<char>((i * 7 + 13) & 0xFF);

        std::vector<char> recv_data(size);
        std::error_code write_ec, read_ec;
        std::size_t write_n = 0, read_n = 0;

        // Writer and reader must run concurrently to avoid deadlock
        // when the payload exceeds the TCP send buffer.
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await capy::write(
                s1, capy::const_buffer(send_data.data(), send_data.size()));
            write_ec = ec;
            write_n  = n;
        };

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await capy::read(
                s2, capy::mutable_buffer(recv_data.data(), recv_data.size()));
            read_ec = ec;
            read_n  = n;
        };

        capy::run_async(ioc.get_executor())(writer());
        capy::run_async(ioc.get_executor())(reader());

        ioc.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, size);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, size);
        BOOST_TEST(send_data == recv_data);

        s1.close();
        s2.close();
    }

    void testBinaryData()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        auto task = [](tcp_socket& a, tcp_socket& b) -> capy::task<> {
            // All 256 byte values
            std::array<unsigned char, 256> send_data;
            for (int i = 0; i < 256; ++i)
                send_data[i] = static_cast<unsigned char>(i);

            auto [ec1, n1] = co_await capy::write(
                a, capy::const_buffer(send_data.data(), send_data.size()));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 256u);

            std::array<unsigned char, 256> recv_data = {};
            auto [ec2, n2]                           = co_await capy::read(
                b, capy::mutable_buffer(recv_data.data(), recv_data.size()));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 256u);
            BOOST_TEST(send_data == recv_data);
        };
        capy::run_async(ioc.get_executor())(task(s1, s2));

        ioc.run();
        s1.close();
        s2.close();
    }

    // Endpoint Query Tests

    void testEndpointsEphemeralPort()
    {
        // Test with ephemeral port (port 0 - OS assigns)
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Bind to loopback with port 0 (ephemeral)
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto listen_ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        if (!listen_ec)
            listen_ec = acc.listen();
        BOOST_TEST(!listen_ec);

        // Acceptor's local endpoint should have a non-zero OS-assigned port
        auto acc_local = acc.local_endpoint();
        BOOST_TEST(acc_local.port() != 0);
        BOOST_TEST(acc_local.is_v4());

        tcp_socket client(ioc);
        tcp_socket server(ioc);
        client.open();

        auto task = [&]() -> capy::task<> {
            // Connect to the acceptor
            auto [ec] = co_await client.connect(acc.local_endpoint());
            BOOST_TEST(!ec);
        };

        auto accept_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(server);
            BOOST_TEST(!ec);
        };

        capy::run_async(ioc.get_executor())(task());
        capy::run_async(ioc.get_executor())(accept_task());

        ioc.run();

        // Client's remote endpoint should equal the endpoint passed to connect()
        BOOST_TEST(client.remote_endpoint() == acc.local_endpoint());

        // Client's local endpoint should have a non-zero OS-assigned port
        BOOST_TEST(client.local_endpoint().port() != 0);
        BOOST_TEST(client.local_endpoint().is_v4());

        // Server's remote endpoint should equal client's local endpoint (peer consistency)
        BOOST_TEST(server.remote_endpoint() == client.local_endpoint());

        // Server's local endpoint should equal client's remote endpoint (peer consistency)
        BOOST_TEST(server.local_endpoint() == client.remote_endpoint());

        client.close();
        server.close();
        acc.close();
    }

    void testEndpointsSpecifiedPort()
    {
        // Test with a specified port number
        io_context ioc(Backend);
        tcp_acceptor acc(ioc);

        // Simple fast LCG random number generator seeded with PID
#if BOOST_COROSIO_POSIX
        std::uint32_t rng_state = static_cast<std::uint32_t>(getpid());
#else
        std::uint32_t rng_state = static_cast<std::uint32_t>(_getpid());
#endif
        auto fast_rand = [&rng_state]() -> std::uint16_t {
            rng_state = rng_state * 1103515245 + 12345;
            return static_cast<std::uint16_t>((rng_state >> 16) & 0x3F) +
                1; // 1-64
        };

        // Try to find an available port outside the ephemeral range
        std::uint16_t test_port = 18080;
        bool found              = false;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            acc.open();
            acc.set_option(socket_option::reuse_address(true));
            if (!acc.bind(endpoint(ipv4_address::loopback(), test_port)) &&
                !acc.listen())
            {
                found = true;
                break;
            }
            acc.close();
            acc = tcp_acceptor(ioc);
            test_port += fast_rand();
        }
        if (!found)
        {
            std::fprintf(
                stderr,
                "testEndpointsSpecifiedPort: failed to find available port "
                "after 100 attempts\n");
            return;
        }

        // Acceptor's local endpoint should have the specified port
        BOOST_TEST(acc.local_endpoint().port() == test_port);

        tcp_socket client(ioc);
        tcp_socket server(ioc);
        client.open();

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), test_port));
            BOOST_TEST(!ec);
        };

        auto accept_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(server);
            BOOST_TEST(!ec);
        };

        capy::run_async(ioc.get_executor())(task());
        capy::run_async(ioc.get_executor())(accept_task());

        ioc.run();

        // Client's remote endpoint should equal the endpoint passed to connect()
        BOOST_TEST(client.remote_endpoint().port() == test_port);
        BOOST_TEST(
            client.remote_endpoint() ==
            endpoint(ipv4_address::loopback(), test_port));

        // Server's local endpoint should have the specified port
        BOOST_TEST(server.local_endpoint().port() == test_port);

        client.close();
        server.close();
        acc.close();
    }

    void testEndpointOnClosedSocket()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);

        // Closed tcp_socket should return default endpoint
        BOOST_TEST(sock.local_endpoint() == endpoint{});
        BOOST_TEST(sock.remote_endpoint() == endpoint{});
        BOOST_TEST(sock.local_endpoint().port() == 0);
        BOOST_TEST(sock.remote_endpoint().port() == 0);
    }

    void testEndpointBeforeConnect()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        // Open but unconnected tcp_socket should return default endpoint
        BOOST_TEST(sock.local_endpoint() == endpoint{});
        BOOST_TEST(sock.remote_endpoint() == endpoint{});

        sock.close();
    }

    void testEndpointsAfterConnectFailure()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open();

        auto task = [&]() -> capy::task<> {
            // Connect to an unreachable address (localhost on unlikely port)
            auto [ec] = co_await sock.connect(endpoint(
                ipv4_address::loopback(), 1)); // Port 1 is typically closed
            // We expect this to fail (connection refused or similar)
            BOOST_TEST(ec);
        };

        capy::run_async(ioc.get_executor())(task());
        ioc.run();

        // After failed connect, endpoints should remain default
        BOOST_TEST(sock.local_endpoint() == endpoint{});
        BOOST_TEST(sock.remote_endpoint() == endpoint{});

        sock.close();
    }

    void testEndpointsMoveConstruct()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Get original endpoints
        auto orig_local  = s1.local_endpoint();
        auto orig_remote = s1.remote_endpoint();

        // Endpoints should be non-default after connection
        BOOST_TEST(orig_local.port() != 0);
        BOOST_TEST(orig_remote.port() != 0);

        // Move construct
        tcp_socket s3(std::move(s1));

        // Moved-from tcp_socket should return default endpoints
        BOOST_TEST(s1.local_endpoint() == endpoint{});
        BOOST_TEST(s1.remote_endpoint() == endpoint{});

        // Moved-to tcp_socket should have original endpoints
        BOOST_TEST(s3.local_endpoint() == orig_local);
        BOOST_TEST(s3.remote_endpoint() == orig_remote);

        s1.close();
        s2.close();
        s3.close();
    }

    void testEndpointsMoveAssign()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Get original endpoints
        auto orig_local  = s1.local_endpoint();
        auto orig_remote = s1.remote_endpoint();

        // Create another tcp_socket to move-assign to
        tcp_socket s3(ioc);

        // Move assign
        s3 = std::move(s1);

        // Moved-from tcp_socket should return default endpoints
        BOOST_TEST(s1.local_endpoint() == endpoint{});
        BOOST_TEST(s1.remote_endpoint() == endpoint{});

        // Moved-to tcp_socket should have original endpoints
        BOOST_TEST(s3.local_endpoint() == orig_local);
        BOOST_TEST(s3.remote_endpoint() == orig_remote);

        s1.close();
        s2.close();
        s3.close();
    }

    void testEndpointsConsistentReads()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Multiple reads should return the same cached values
        auto local1 = s1.local_endpoint();
        auto local2 = s1.local_endpoint();
        auto local3 = s1.local_endpoint();
        BOOST_TEST(local1 == local2);
        BOOST_TEST(local2 == local3);

        auto remote1 = s1.remote_endpoint();
        auto remote2 = s1.remote_endpoint();
        auto remote3 = s1.remote_endpoint();
        BOOST_TEST(remote1 == remote2);
        BOOST_TEST(remote2 == remote3);

        s1.close();
        s2.close();
    }

    void testEndpointsAfterCloseAndReopen()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Get endpoints while connected
        auto orig_local  = s1.local_endpoint();
        auto orig_remote = s1.remote_endpoint();
        BOOST_TEST(orig_local.port() != 0);
        BOOST_TEST(orig_remote.port() != 0);

        // Close the tcp_socket
        s1.close();

        // After close, endpoints should be default
        BOOST_TEST(s1.local_endpoint() == endpoint{});
        BOOST_TEST(s1.remote_endpoint() == endpoint{});

        // Reopen the tcp_socket
        s1.open();

        // After reopen (but before connect), endpoints should still be default
        BOOST_TEST(s1.local_endpoint() == endpoint{});
        BOOST_TEST(s1.remote_endpoint() == endpoint{});

        s1.close();
        s2.close();
    }

    void run()
    {
        testConstruction();
        testOpen();
        testBind();
        testBindThenConnect();
        testBindV6();
        testBindClosedSocketThrows();
        testBindAddressInUse();
        testBindNonLocalAddress();
        testMoveConstruct();
        testMoveAssign();

        // Basic I/O
        testReadSome();
        testWriteSome();
        testPartialRead();
        testSequentialReadWrite();
        testBidirectionalSimultaneous();

        // Buffer variations
        testEmptyBuffer();
        testSmallBuffer();
        testLargeBuffer();

        // EOF and closure
        testReadAfterPeerClose();
        testWriteAfterPeerClose();

        // Shutdown
        testShutdownSend();
        testShutdownReceive();
        testShutdownOnClosedSocket();
        testShutdownBothSendDirection();

        // Cancellation
        testCancelRead();
        testCloseWhileReading();
        testStopTokenCancellation();

        // Socket options
        testNoDelay();
        testKeepAlive();
        testReceiveBufferSize();
        testSendBufferSize();
        testLinger();
        testLingerValidation();
        testSocketOptionsOnConnectedSocket();
        testGenericSetGetOption();
        testGenericBufferOption();
        testGenericLingerOption();
        testOptionAssignmentOperators();

        // Composed operations
        testReadFull();
        testWriteFull();
        testReadString();
        testReadPartialEOF();

        // Data integrity
        testLargeTransfer();
        testBinaryData();

        // Endpoint queries
        testEndpointsEphemeralPort();
        testEndpointsSpecifiedPort();
        testEndpointOnClosedSocket();
        testEndpointBeforeConnect();
        testEndpointsAfterConnectFailure();
        testEndpointsMoveConstruct();
        testEndpointsMoveAssign();
        testEndpointsConsistentReads();
        testEndpointsAfterCloseAndReopen();

        // IPv6 and lazy-open
        testConnectV6();
        testLazyOpenV4();
        testLazyOpenPreservesExistingSocket();
        testV6ReadWrite();

        // v6_only socket option
        testV6OnlySocketOption();
        testDualStackConnect();
    }

    void testConnectV6()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        if (!ec)
            ec = acc.listen();
        BOOST_TEST(!ec);
        BOOST_TEST(acc.local_endpoint().is_v6());

        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc);
        tcp_socket s2(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, s1, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(s2, endpoint(ipv6_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        BOOST_TEST(s1.local_endpoint().is_v6());
        BOOST_TEST(s1.remote_endpoint().is_v6());
        BOOST_TEST(s2.local_endpoint().is_v6());
        BOOST_TEST(s2.remote_endpoint().is_v6());

        s1.close();
        s2.close();
        acc.close();
    }

    void testLazyOpenV4()
    {
        // connect() on a closed socket should auto-open with AF_INET
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        if (!ec)
            ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc);
        tcp_socket s2(ioc);
        // Do NOT call s2.open() — connect() should open it

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, s1, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(s2, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST(s2.is_open());

        s1.close();
        s2.close();
        acc.close();
    }

    void testLazyOpenPreservesExistingSocket()
    {
        // If socket is already open, connect() should not re-open it.
        // Verify that a socket option set before connect() is retained.
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv4_address::loopback(), 0));
        if (!ec)
            ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc);
        tcp_socket s2(ioc);
        s2.open();
        s2.set_option(socket_option::no_delay(true));
        BOOST_TEST(s2.get_option<socket_option::no_delay>());

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, s1, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(s2, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        // Socket option should be preserved across connect
        BOOST_TEST(s2.get_option<socket_option::no_delay>());

        s1.close();
        s2.close();
        acc.close();
    }

    void testV6ReadWrite()
    {
        io_context ioc(Backend);

        tcp_acceptor acc(ioc);
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::loopback(), 0));
        if (!ec)
            ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc);
        tcp_socket s2(ioc);

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, s1, accept_ec, accept_done));

        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(s2, endpoint(ipv6_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        // Round-trip data over IPv6
        std::string const msg = "hello IPv6";
        bool write_done       = false;
        bool read_done        = false;
        std::error_code write_ec, read_ec;
        std::size_t write_n = 0, read_n = 0;
        char buf[64]{};

        capy::run_async(ex)(
            [](tcp_socket& s, char const* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.write_some(capy::const_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s2, msg.data(), msg.size(), write_ec, write_n, write_done));

        capy::run_async(ex)(
            [](tcp_socket& s, char* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.read_some(capy::mutable_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s1, buf, sizeof(buf), read_ec, read_n, read_done));

        ioc.run();

        BOOST_TEST(write_done);
        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, msg.size());
        BOOST_TEST(read_done);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, msg.size());
        BOOST_TEST_EQ(std::string_view(buf, read_n), std::string_view(msg));

        s1.close();
        s2.close();
        acc.close();
    }

    void testV6OnlySocketOption()
    {
        io_context ioc(Backend);
        tcp_socket sock(ioc);
        sock.open(tcp::v6()); // IPv6

        // Default is v6only=true (kernel default after open_socket sets it)
        BOOST_TEST_EQ(sock.get_option<socket_option::v6_only>().value(), true);

        sock.set_option(socket_option::v6_only(false));
        BOOST_TEST_EQ(sock.get_option<socket_option::v6_only>().value(), false);

        sock.set_option(socket_option::v6_only(true));
        BOOST_TEST_EQ(sock.get_option<socket_option::v6_only>().value(), true);

        sock.close();
    }

    void testDualStackConnect()
    {
        io_context ioc(Backend);

        // Dual-stack listener (v6only=false is the default)
        tcp_acceptor acc(ioc);
        acc.open(tcp::v6());
        acc.set_option(socket_option::reuse_address(true));
        auto ec = acc.bind(endpoint(ipv6_address::any(), 0));
        if (!ec)
            ec = acc.listen();
        BOOST_TEST(!ec);
        auto port = acc.local_endpoint().port();

        tcp_socket s1(ioc);
        tcp_socket s2(ioc);
        s2.open(tcp::v6()); // IPv6 socket
        s2.set_option(socket_option::v6_only(false));

        bool accept_done  = false;
        bool connect_done = false;
        std::error_code accept_ec, connect_ec;

        auto ex = ioc.get_executor();
        capy::run_async(ex)(
            [](tcp_acceptor& a, tcp_socket& s, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.accept(s);
                ec_out    = ec;
                done      = true;
            }(acc, s1, accept_ec, accept_done));

        // IPv6 dual-stack socket connects to IPv4 loopback —
        // connect maps to ::ffff:127.0.0.1 automatically
        capy::run_async(ex)(
            [](tcp_socket& s, endpoint ep, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await s.connect(ep);
                ec_out    = ec;
                done      = true;
            }(s2, endpoint(ipv4_address::loopback(), port), connect_ec,
                           connect_done));

        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);

        // Accepted peer has IPv6 endpoint (IPv4-mapped)
        BOOST_TEST(s1.remote_endpoint().is_v6());

        s1.close();
        s2.close();
        acc.close();
    }
};

COROSIO_BACKEND_TESTS(tcp_socket_test, "boost.corosio.tcp_socket")

} // namespace boost::corosio
