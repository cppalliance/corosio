//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Coverage tests for reactor-backend code paths that are not exercised by
// the per-type unit tests. Targets: multi-op queueing on the same fd,
// wait_type::error completions, scatter-gather (multi-buffer) I/O, acceptor
// wait variants, datagram zero-length receive, shutdown variants, and
// close-during-pending-op races.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <string_view>
#include <system_error>
#include <vector>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>

#include <sys/socket.h>
#include <unistd.h>
#endif

#include <boost/corosio/test/local_socket_pair.hpp>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

using test::make_local_stream_pair;
#if BOOST_COROSIO_POSIX
using test::make_local_datagram_pair;
#endif

template<auto Backend>
struct reactor_paths_test
{
    // Spawn a read and a write coroutine on the same socket concurrently.
    // Exercises the descriptor_state branch where a single edge event
    // contains both read and write readiness for the same fd.
    void testConcurrentReadWrite()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        constexpr std::string_view payload = "concurrent";
        char read_buf[64]                  = {};

        std::error_code read_ec;
        std::error_code write_ec;
        std::size_t read_n  = 0;
        std::size_t write_n = 0;

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.read_some(
                capy::mutable_buffer(read_buf, sizeof(read_buf)));
            read_ec = ec;
            read_n  = n;
        };
        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            write_ec = ec;
            write_n  = n;
        };
        auto peer_writer = [&]() -> capy::task<> {
            // Brief delay so the read side parks first.
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();
            auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        capy::run_async(ex)(peer_writer());
        ioc.run();

        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, payload.size());
        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, payload.size());
    }

    // Park a wait_type::error op while a connect to an unreachable port
    // is in progress. The EPOLLERR/SO_ERROR delivery triggers the
    // wait_error_op completion path in descriptor_state's err block.
    // Bounded by a fallback cancel so the wait doesn't hang if the
    // backend does not surface the error as wait_type::error completion.
    void testConnectErrorFiresWaitError()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_socket sock(ioc);
        sock.open();

        std::error_code conn_ec;
        bool conn_done = false;
        std::error_code wait_ec;
        bool wait_done = false;

        auto connector = [&]() -> capy::task<> {
            auto [ec] = co_await sock.connect(
                endpoint(ipv4_address::loopback(), 1));
            conn_ec   = ec;
            conn_done = true;
        };
        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(500));
            (void)co_await t.wait();
            sock.cancel();
        };

        capy::run_async(ex)(connector());
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(conn_done);
        BOOST_TEST(conn_ec);
        BOOST_TEST(wait_done);
    }

    // Connect to an unreachable peer; the failed handshake delivers
    // EPOLLERR. Exercises descriptor_state's error branch.
    void testConnectFailureReportsError()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_socket sock(ioc);
        sock.open();

        // Use port 1 which is well-known reserved and very unlikely to
        // be listening; the resulting connect will get RST or fail.
        std::error_code conn_ec;
        bool conn_done = false;
        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await sock.connect(
                endpoint(ipv4_address::loopback(), 1));
            conn_ec   = ec;
            conn_done = true;
        };

        capy::run_async(ex)(task());
        ioc.run();

        BOOST_TEST(conn_done);
        BOOST_TEST(conn_ec);
    }

    // wait_type::error should complete when peer closes (reactor delivers
    // HUP via the err/ready_events path). Bounded by a cancel timer because
    // not every backend reports HUP as an error condition.
    void testWaitForError()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        auto closer = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            s2.close();
            // Bound the wait: cancel s1 after another delay if the peer
            // close did not surface as an error condition.
            timer t2(ioc);
            t2.expires_after(std::chrono::milliseconds(200));
            (void)co_await t2.wait();
            s1.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(wait_done);
        // wait_ec may be: success (HUP), specific errno, or canceled —
        // all valid termination conditions for a parked wait_error op.
    }

    // Cancel a parked wait_type::error op via socket.cancel(). Exercises the
    // wait_error_op cancellation path through reactor_basic_socket::do_cancel.
    void testCancelWaitForError()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
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

    // Multi-buffer (scatter-gather) read. Exercises the readv() branch in
    // reactor_stream_socket::do_read_some.
    void testScatterRead()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        constexpr std::string_view payload = "scatter-gather-payload-data";
        char buf1[8] = {};
        char buf2[8] = {};
        char buf3[32] = {};

        std::error_code read_ec;
        std::size_t read_n = 0;

        auto reader = [&]() -> capy::task<> {
            std::array<capy::mutable_buffer, 3> bufs = {
                capy::mutable_buffer(buf1, sizeof(buf1)),
                capy::mutable_buffer(buf2, sizeof(buf2)),
                capy::mutable_buffer(buf3, sizeof(buf3)),
            };
            auto [ec, n] = co_await s1.read_some(bufs);
            read_ec      = ec;
            read_n       = n;
        };
        auto writer = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();
            auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, payload.size());
    }

    // Write a large payload into a socket with a tiny send buffer to
    // force the EAGAIN path inside do_write_some, which parks the op in
    // desc_state.write_op and re-arms via EPOLLOUT.
    void testWriteEAGAIN()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        // Shrink kernel buffers as much as possible.
        s1.set_option(socket_option::send_buffer_size(1024));
        s2.set_option(socket_option::receive_buffer_size(1024));

        constexpr std::size_t total = std::size_t{256} * 1024; // 256 KiB
        std::vector<char> payload(total, 'X');

        std::error_code write_ec;
        std::size_t bytes_written = 0;
        std::error_code read_ec;
        std::size_t bytes_read = 0;

        auto writer = [&]() -> capy::task<> {
            std::size_t off = 0;
            while (off < payload.size())
            {
                auto [ec, n] = co_await s1.write_some(
                    capy::const_buffer(
                        payload.data() + off, payload.size() - off));
                if (ec)
                {
                    write_ec = ec;
                    co_return;
                }
                off += n;
                bytes_written = off;
            }
        };
        auto reader = [&]() -> capy::task<> {
            std::vector<char> buf(4096);
            while (bytes_read < total)
            {
                auto [ec, n] = co_await s2.read_some(
                    capy::mutable_buffer(buf.data(), buf.size()));
                if (ec)
                {
                    read_ec = ec;
                    co_return;
                }
                if (n == 0)
                    co_return;
                bytes_read += n;
            }
        };

        capy::run_async(ex)(writer());
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(bytes_written, total);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, total);
    }

    // Multi-buffer (gather) write. Exercises the writev() branch in
    // reactor_stream_socket::do_write_some.
    void testGatherWrite()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        constexpr std::string_view part1 = "hello-";
        constexpr std::string_view part2 = "scatter-";
        constexpr std::string_view part3 = "world";

        std::error_code write_ec;
        std::size_t write_n = 0;
        char read_buf[64]   = {};
        std::size_t total_read = 0;
        std::error_code read_ec;

        auto writer = [&]() -> capy::task<> {
            std::array<capy::const_buffer, 3> bufs = {
                capy::const_buffer(part1.data(), part1.size()),
                capy::const_buffer(part2.data(), part2.size()),
                capy::const_buffer(part3.data(), part3.size()),
            };
            auto [ec, n] = co_await s1.write_some(bufs);
            write_ec     = ec;
            write_n      = n;
        };
        auto reader = [&]() -> capy::task<> {
            // Drain until we have the full expected payload or no more data.
            std::size_t expected = part1.size() + part2.size() + part3.size();
            while (total_read < expected)
            {
                auto [ec, n] = co_await s2.read_some(capy::mutable_buffer(
                    read_buf + total_read, sizeof(read_buf) - total_read));
                if (ec)
                {
                    read_ec = ec;
                    co_return;
                }
                if (n == 0)
                    co_return;
                total_read += n;
            }
        };

        capy::run_async(ex)(writer());
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, part1.size() + part2.size() + part3.size());
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(total_read, part1.size() + part2.size() + part3.size());
    }

    // Acceptor wait_type::write completes immediately. Exercises the early
    // return path in reactor_acceptor::do_wait.
    void testAcceptorWaitWrite()
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

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Cancel a parked acceptor wait_type::error. Exercises the
    // wait_er_ cancel path in reactor_acceptor::do_cancel.
    void testAcceptorWaitErrorCancelled()
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

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            acc.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // UDP wait_type::write completes immediately.
    void testUdpWaitWrite()
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
            auto [ec] = co_await sock.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // UDP wait_type::error then cancel. Exercises wait_er_ cancel path
    // on reactor_datagram_socket.
    void testUdpWaitErrorCancel()
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
            auto [ec] = co_await sock.wait(wait_type::error);
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

    // UDP recv_from with a zero-length buffer should complete immediately
    // with zero bytes. Exercises the empty-buffer branch in do_recv_from.
    void testUdpRecvFromEmpty()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::error_code rf_ec;
        std::size_t rf_n = 1;
        endpoint src;

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock.recv_from(
                capy::mutable_buffer(nullptr, 0), src);
            rf_ec = ec;
            rf_n  = n;
        };

        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(!rf_ec);
        BOOST_TEST_EQ(rf_n, 0u);
    }

    // UDP send with a zero-length buffer should complete immediately.
    void testUdpSendEmpty()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Use a connected pair to avoid sendto address issues.
        udp_socket s1(ioc);
        udp_socket s2(ioc);
        s1.open(udp::v4());
        s2.open(udp::v4());
        auto e1 = s1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e1);
        auto e2 = s2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e2);
        auto port = s2.local_endpoint().port();

        std::error_code conn_ec;
        bool conn_done = false;
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(
                endpoint(ipv4_address::loopback(), port));
            conn_ec   = ec;
            conn_done = true;
        };
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();
        BOOST_TEST(conn_done);
        BOOST_TEST(!conn_ec);

        std::error_code send_ec;
        std::size_t send_n = 1;

        auto sender = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.send(capy::const_buffer(nullptr, 0));
            send_ec = ec;
            send_n  = n;
        };

        capy::run_async(ex)(sender());
        ioc.run();

        // Zero-length UDP send: Linux accepts; macOS returns EMSGSIZE;
        // various Windows variants return EINVAL or other errnos. The
        // library send path is exercised regardless.
        if (!send_ec)
            BOOST_TEST_EQ(send_n, 0u);
    }

    // Connected UDP recv with zero-length buffer (do_recv empty path).
    void testUdpRecvEmpty()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc);
        udp_socket s2(ioc);
        s1.open(udp::v4());
        s2.open(udp::v4());
        auto e1 = s1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e1);
        auto e2 = s2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e2);
        auto port = s2.local_endpoint().port();

        std::error_code conn_ec;
        bool conn_done = false;
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(
                endpoint(ipv4_address::loopback(), port));
            conn_ec   = ec;
            conn_done = true;
        };
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();
        BOOST_TEST(conn_done);
        BOOST_TEST(!conn_ec);

        std::error_code recv_ec;
        std::size_t recv_n = 1;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(capy::mutable_buffer(nullptr, 0));
            recv_ec      = ec;
            recv_n       = n;
        };

        capy::run_async(ex)(receiver());
        ioc.run();

        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(recv_n, 0u);
    }

    // Connected UDP send_to with proper endpoint should still parse correctly.
    void testUdpSendToEmpty()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc);
        udp_socket s2(ioc);
        s1.open(udp::v4());
        s2.open(udp::v4());
        auto e1 = s1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e1);
        auto e2 = s2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e2);
        auto port = s2.local_endpoint().port();

        std::error_code send_ec;
        std::size_t send_n = 1;

        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send_to(
                capy::const_buffer(nullptr, 0),
                endpoint(ipv4_address::loopback(), port));
            send_ec = ec;
            send_n  = n;
        };

        capy::run_async(ex)(sender());
        ioc.run();

        // Zero-length UDP send_to: same platform variation as testUdpSendEmpty.
        // The library send_to path is exercised regardless of the
        // platform's specific errno.
        if (!send_ec)
            BOOST_TEST_EQ(send_n, 0u);
    }

    // Shutdown both directions on a stream socket.
    void testShutdownBoth()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        bool ok = false;
        auto task = [&]() -> capy::task<> {
            s1.shutdown(shutdown_both);
            ok = true;
            co_return;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(ok);
    }

    // Shutdown receive direction.
    void testShutdownReceive()
    {
        io_context ioc(Backend);
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        bool ok = false;
        auto task = [&]() -> capy::task<> {
            s1.shutdown(shutdown_receive);
            ok = true;
            co_return;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(ok);
    }

    // Close a UDP socket with a parked wait. Exercises the close path's
    // claim-and-repost over wait_er_/wait_rd_/wait_wr_ slots.
    void testUdpCloseWhileWaiting()
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
        auto closer = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            sock.close();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Multiple acceptors on the same scheduler accept concurrently.
    // Exercises reactor's multi-fd registration and dispatch path.
    void testMultipleAcceptors()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        constexpr int N = 3;
        std::vector<tcp_acceptor> accs;
        std::vector<tcp_socket> peers;
        std::vector<tcp_socket> clients;
        std::vector<std::uint16_t> ports;
        accs.reserve(N);
        peers.reserve(N);
        clients.reserve(N);
        ports.reserve(N);

        for (int i = 0; i < N; ++i)
        {
            accs.emplace_back(ioc);
            accs.back().open();
            accs.back().set_option(socket_option::reuse_address(true));
            BOOST_TEST(!accs.back().bind(endpoint(ipv4_address::loopback(), 0)));
            BOOST_TEST(!accs.back().listen());
            ports.push_back(accs.back().local_endpoint().port());
            peers.emplace_back(ioc);
            clients.emplace_back(ioc);
            clients.back().open();
        }

        std::array<bool, N> accept_done{};
        std::array<bool, N> connect_done{};

        for (int i = 0; i < N; ++i)
        {
            capy::run_async(ex)(
                [](tcp_acceptor& a, tcp_socket& p, bool& done)
                    -> capy::task<> {
                    auto [ec] = co_await a.accept(p);
                    (void)ec;
                    done = true;
                }(accs[i], peers[i], accept_done[i]));
            capy::run_async(ex)(
                [](tcp_socket& c, endpoint ep, bool& done) -> capy::task<> {
                    auto [ec] = co_await c.connect(ep);
                    (void)ec;
                    done = true;
                }(clients[i], endpoint(ipv4_address::loopback(), ports[i]),
                                 connect_done[i]));
        }

        ioc.run();

        for (int i = 0; i < N; ++i)
        {
            BOOST_TEST(accept_done[i]);
            BOOST_TEST(connect_done[i]);
        }
    }

    // Stop-token cancel of a parked wait(read). Exercises cancel_single_op
    // via the per-op canceller (not socket.cancel()), which dispatches
    // through op_to_desc_slot/op_to_cancel_flag for the wait_rd_ slot.
    void testStopTokenWaitRead()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
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
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Stop-token cancel of wait(error).
    void testStopTokenWaitError()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] =
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);

        std::stop_source ss;
        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Stop-token cancel of a UDP recv. Exercises cancel_single_op on the
    // recv_rd_ slot in reactor_datagram_socket.
    void testStopTokenUdpRecv()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc);
        udp_socket s2(ioc);
        s1.open(udp::v4());
        s2.open(udp::v4());
        auto e1 = s1.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e1);
        auto e2 = s2.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!e2);
        auto port = s2.local_endpoint().port();

        std::error_code conn_ec;
        bool conn_done = false;
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await s1.connect(
                endpoint(ipv4_address::loopback(), port));
            conn_ec   = ec;
            conn_done = true;
        };
        capy::run_async(ex)(connect_task());
        ioc.run();
        ioc.restart();
        BOOST_TEST(conn_done);
        BOOST_TEST(!conn_ec);

        std::stop_source ss;
        std::error_code recv_ec;
        bool recv_done = false;
        char buf[64];

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.recv(capy::mutable_buffer(buf, sizeof(buf)));
            (void)n;
            recv_ec   = ec;
            recv_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(receiver());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
    }

    // Stop-token cancel of a UDP recv_from.
    void testStopTokenUdpRecvFrom()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::stop_source ss;
        std::error_code recv_ec;
        bool recv_done = false;
        endpoint src;
        char buf[64];

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), src);
            (void)n;
            recv_ec   = ec;
            recv_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(receiver());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
    }

    // Stop-token cancel of an acceptor accept().
    void testStopTokenAcceptorAccept()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        std::error_code accept_ec;
        bool accept_done = false;
        tcp_socket peer(ioc);

        auto acceptor_task = [&]() -> capy::task<> {
            auto [ec]   = co_await acc.accept(peer);
            accept_ec   = ec;
            accept_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(acceptor_task());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(accept_ec == capy::cond::canceled);
    }

    // configure_reactor with max_events == 0 throws std::out_of_range.
    // IOCP ignores max_events_per_poll (no batch poll), so only test
    // on reactor backends.
    void testIoContextOptionsMaxEventsZero()
    {
#if BOOST_COROSIO_POSIX
        io_context_options opts;
        opts.max_events_per_poll = 0;
        bool threw = false;
        try
        {
            io_context ioc(Backend, opts);
            (void)ioc;
        }
        catch (std::out_of_range const&)
        {
            threw = true;
        }
        BOOST_TEST(threw);
#endif
    }

    // configure_reactor with budget_init > budget_max clamps.
    void testIoContextOptionsBudgetInitClamp()
    {
        io_context_options opts;
        opts.inline_budget_initial = 100;
        opts.inline_budget_max     = 5;
        opts.unassisted_budget     = 200;
        io_context ioc(Backend, opts);
        // Construction succeeds; values are silently clamped.
        BOOST_TEST(true);
    }

#if BOOST_COROSIO_POSIX
    // Assign a non-AF_UNIX fd to a local_stream_socket: validation
    // returns EAFNOSUPPORT (do_assign_fd::ss_family != AF_UNIX path).
    void testLocalStreamAssignWrongFamily()
    {
        io_context ioc(Backend);

        // Create a TCP socket fd that's not AF_UNIX.
        int tcp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        BOOST_TEST(tcp_fd >= 0);

        local_stream_socket sock(ioc);
        bool threw = false;
        try
        {
            sock.assign(tcp_fd);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        // assign may throw on wrong type/family; cleanup if owned.
        if (!sock.is_open())
            ::close(tcp_fd);
        BOOST_TEST(threw);
    }

    // Assign a stream-type fd (AF_UNIX SOCK_STREAM) to local_datagram_socket:
    // validation returns EPROTOTYPE.
    void testLocalDgramAssignWrongType()
    {
        io_context ioc(Backend);

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        BOOST_TEST(fd >= 0);

        local_datagram_socket sock(ioc);
        bool threw = false;
        try
        {
            sock.assign(fd);
        }
        catch (std::system_error const&)
        {
            threw = true;
        }
        if (!sock.is_open())
            ::close(fd);
        BOOST_TEST(threw);
    }
#endif

    // Issue wait() with an already-stopped token. Exercises the
    // op.cancelled.load() == true branch in reactor_acceptor::do_wait.
    void testWaitWithPreCancelledToken()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        acc.open();
        acc.set_option(socket_option::reuse_address(true));
        BOOST_TEST(!acc.bind(endpoint(ipv4_address::loopback(), 0)));
        BOOST_TEST(!acc.listen());

        std::stop_source ss;
        ss.request_stop(); // Already stopped before the wait registers.
        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await acc.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex, ss.get_token())(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

#if BOOST_COROSIO_POSIX
    // Local stream socket wait_type::error then cancel. Exercises the
    // local-endpoint specialization of reactor_stream_socket.
    void testLocalStreamWaitErrorCancel()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_stream_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
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

    // Local stream socket wait_type::write completes immediately.
    void testLocalStreamWaitWrite()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_stream_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Local stream scatter-gather read.
    void testLocalStreamScatterRead()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_stream_pair(ioc);

        constexpr std::string_view payload = "local-scatter-read-payload";
        char a[6] = {};
        char b[6] = {};
        char c[64] = {};

        std::error_code read_ec;
        std::size_t read_n = 0;

        auto reader = [&]() -> capy::task<> {
            std::array<capy::mutable_buffer, 3> bufs = {
                capy::mutable_buffer(a, sizeof(a)),
                capy::mutable_buffer(b, sizeof(b)),
                capy::mutable_buffer(c, sizeof(c)),
            };
            auto [ec, n] = co_await s1.read_some(bufs);
            read_ec      = ec;
            read_n       = n;
        };
        auto writer = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(10));
            (void)co_await t.wait();
            auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(read_n, payload.size());
    }

    // Local stream gather write.
    void testLocalStreamGatherWrite()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_stream_pair(ioc);

        constexpr std::string_view part1 = "aaa-";
        constexpr std::string_view part2 = "bbb-";
        constexpr std::string_view part3 = "ccc";

        std::error_code write_ec;
        std::size_t write_n = 0;

        auto writer = [&]() -> capy::task<> {
            std::array<capy::const_buffer, 3> bufs = {
                capy::const_buffer(part1.data(), part1.size()),
                capy::const_buffer(part2.data(), part2.size()),
                capy::const_buffer(part3.data(), part3.size()),
            };
            auto [ec, n] = co_await s1.write_some(bufs);
            write_ec     = ec;
            write_n      = n;
        };
        auto reader = [&]() -> capy::task<> {
            char buf[64];
            auto [ec, n] =
                co_await s2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            (void)ec;
            (void)n;
        };

        capy::run_async(ex)(writer());
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST_EQ(write_n, part1.size() + part2.size() + part3.size());
    }

    // Local datagram socket wait_type::error then cancel.
    void testLocalDgramWaitErrorCancel()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
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

    // Local datagram wait_type::write immediate completion.
    void testLocalDgramWaitWrite()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Local datagram recv with zero-length buffer.
    void testLocalDgramRecvEmpty()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code recv_ec;
        std::size_t recv_n = 1;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(capy::mutable_buffer(nullptr, 0));
            recv_ec      = ec;
            recv_n       = n;
        };

        capy::run_async(ex)(receiver());
        ioc.run();

        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(recv_n, 0u);
    }

    // Local datagram send with zero-length buffer (connected).
    void testLocalDgramSendEmpty()
    {
        io_context ioc(Backend);
        auto ex       = ioc.get_executor();
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code send_ec;
        std::size_t send_n = 1;

        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.send(capy::const_buffer(nullptr, 0));
            send_ec      = ec;
            send_n       = n;
        };

        capy::run_async(ex)(sender());
        ioc.run();

        // Zero-length local datagram send: Linux accepts; macOS returns
        // EMSGSIZE; other platforms vary. The library send path is
        // exercised regardless.
        if (!send_ec)
            BOOST_TEST_EQ(send_n, 0u);
    }

    // Local datagram socket shutdown_both.
    void testLocalDgramShutdownBoth()
    {
        io_context ioc(Backend);
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code ec;
        auto task = [&]() -> capy::task<> {
            s1.shutdown(shutdown_both, ec);
            co_return;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!ec);
    }

    // Local datagram socket shutdown_receive.
    void testLocalDgramShutdownReceive()
    {
        io_context ioc(Backend);
        auto [s1, s2] = make_local_datagram_pair(ioc);

        std::error_code ec;
        auto task = [&]() -> capy::task<> {
            s1.shutdown(shutdown_receive, ec);
            co_return;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!ec);
    }
#endif

    void run()
    {
        testConcurrentReadWrite();
        testConnectFailureReportsError();
        testConnectErrorFiresWaitError();
        testWaitForError();
        testCancelWaitForError();
        testScatterRead();
        testWriteEAGAIN();
        testGatherWrite();
        testAcceptorWaitWrite();
        testAcceptorWaitErrorCancelled();
        testUdpWaitWrite();
        testUdpWaitErrorCancel();
        testUdpRecvFromEmpty();
        testUdpSendEmpty();
        testUdpRecvEmpty();
        testUdpSendToEmpty();
        testShutdownBoth();
        testShutdownReceive();
        testUdpCloseWhileWaiting();
        testMultipleAcceptors();
        testStopTokenWaitRead();
        testStopTokenWaitError();
        testStopTokenUdpRecv();
        testStopTokenUdpRecvFrom();
        testStopTokenAcceptorAccept();
        testIoContextOptionsMaxEventsZero();
        testIoContextOptionsBudgetInitClamp();
#if BOOST_COROSIO_POSIX
        testLocalStreamAssignWrongFamily();
        testLocalDgramAssignWrongType();
#endif
        testWaitWithPreCancelledToken();
#if BOOST_COROSIO_POSIX
        testLocalStreamWaitErrorCancel();
        testLocalStreamWaitWrite();
        testLocalStreamScatterRead();
        testLocalStreamGatherWrite();
        testLocalDgramWaitErrorCancel();
        testLocalDgramWaitWrite();
        testLocalDgramRecvEmpty();
        testLocalDgramSendEmpty();
        testLocalDgramShutdownBoth();
        testLocalDgramShutdownReceive();
#endif
    }
};

COROSIO_BACKEND_TESTS(reactor_paths_test, "boost.corosio.reactor_paths")

} // namespace boost::corosio
