//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/local_datagram_socket.hpp>

#include <boost/corosio/detail/platform.hpp>

// AF_UNIX SOCK_DGRAM is POSIX-only in practice. Windows added AF_UNIX
// in Win10 1803 but never SOCK_DGRAM over it, so WSASocket fails on
// the very first open() and every test in this file would throw.
// Keep the entire suite POSIX-gated until Windows kernel support lands.
#if BOOST_COROSIO_POSIX

#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/test/temp_path.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct local_datagram_socket_test
{
    void testConstruction()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testOpen()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        sock.open();
        BOOST_TEST_EQ(sock.is_open(), true);

        sock.close();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testMove()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc);
        s1.open();
        BOOST_TEST_EQ(s1.is_open(), true);

        local_datagram_socket s2(std::move(s1));
        BOOST_TEST_EQ(s2.is_open(), true);
        BOOST_TEST_EQ(s1.is_open(), false);
    }

    void testSendRecvConnected()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        auto ex = ioc.get_executor();

        char const msg[] = "dgram test";
        char buf[64]     = {};
        std::error_code send_ec, recv_ec;
        std::size_t sent = 0, recvd = 0;
        bool send_done = false, recv_done = false;

        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.send(capy::const_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s1, msg, std::strlen(msg), send_ec, sent, send_done));

        capy::run_async(ex)(
            [](local_datagram_socket& s, char* data, std::size_t len,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] =
                    co_await s.recv(capy::mutable_buffer(data, len));
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s2, buf, sizeof(buf), recv_ec, recvd, recv_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(send_done, true);
        BOOST_TEST_EQ(!send_ec, true);
        BOOST_TEST_EQ(sent, std::strlen(msg));
        BOOST_TEST_EQ(recv_done, true);
        BOOST_TEST_EQ(!recv_ec, true);
        BOOST_TEST_EQ(recvd, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf, recvd), std::string(msg));
    }

    void testExplicitBind()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        sock.open();

        test::temp_socket_dir tmp;
        auto path = tmp.path();
        auto ec   = sock.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);
    }

    void testSendToRecvFrom()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        auto path1 = tmp1.path();
        auto path2 = tmp2.path();

        local_datagram_socket s1(ioc);
        local_datagram_socket s2(ioc);
        s1.open();
        s2.open();

        auto ec1 = s1.bind(local_endpoint(path1));
        auto ec2 = s2.bind(local_endpoint(path2));
        BOOST_TEST_EQ(!ec1, true);
        BOOST_TEST_EQ(!ec2, true);

        char const msg[] = "sendto test";
        char buf[64]     = {};
        std::error_code send_ec, recv_ec;
        std::size_t sent = 0, recvd = 0;
        bool send_done = false, recv_done = false;
        local_endpoint source;

        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* data, std::size_t len,
               local_endpoint dest,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.send_to(
                    capy::const_buffer(data, len), dest);
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s1, msg, std::strlen(msg), local_endpoint(path2),
              send_ec, sent, send_done));

        capy::run_async(ex)(
            [](local_datagram_socket& s, char* data, std::size_t len,
               local_endpoint& source_out,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.recv_from(
                    capy::mutable_buffer(data, len), source_out);
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s2, buf, sizeof(buf), source, recv_ec, recvd, recv_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(send_done, true);
        BOOST_TEST_EQ(!send_ec, true);
        BOOST_TEST_EQ(sent, std::strlen(msg));
        BOOST_TEST_EQ(recv_done, true);
        BOOST_TEST_EQ(!recv_ec, true);
        BOOST_TEST_EQ(recvd, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf, recvd), std::string(msg));

        // Source endpoint should be the sender's bound path
        BOOST_TEST_EQ(source.path(), path1);
    }

    void testBindFailure()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        sock.open();

        // Bind to a path under a nonexistent directory
        auto ec = sock.bind(local_endpoint("/tmp/nonexistent_dir_corosio/sock"));
        BOOST_TEST_EQ(!!ec, true);
    }

    void testDatagramBoundary()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");
        auto ex = ioc.get_executor();

        // Send two messages of different sizes, verify they
        // arrive as distinct datagrams (not merged like a stream).
        // Use a single coroutine per socket to avoid concurrent
        // same-type operations (documented as unsafe).
        char const msg1[] = "short";
        char const msg2[] = "a longer message";
        char buf1[64] = {};
        char buf2[64] = {};
        std::error_code send_ec1, send_ec2, recv_ec1, recv_ec2;
        std::size_t sent1 = 0, sent2 = 0, recvd1 = 0, recvd2 = 0;
        bool done = false;

        capy::run_async(ex)(
            [](local_datagram_socket& sender,
               local_datagram_socket& receiver,
               char const* m1, std::size_t m1_len,
               char const* m2, std::size_t m2_len,
               char* b1, std::size_t b1_len,
               char* b2, std::size_t b2_len,
               std::error_code& se1, std::size_t& sn1,
               std::error_code& se2, std::size_t& sn2,
               std::error_code& re1, std::size_t& rn1,
               std::error_code& re2, std::size_t& rn2,
               bool& d) -> capy::task<> {
                // Send both messages sequentially
                {
                    auto [ec, n] = co_await sender.send(
                        capy::const_buffer(m1, m1_len));
                    se1 = ec; sn1 = n;
                }
                {
                    auto [ec, n] = co_await sender.send(
                        capy::const_buffer(m2, m2_len));
                    se2 = ec; sn2 = n;
                }
                // Receive both messages sequentially
                {
                    auto [ec, n] = co_await receiver.recv(
                        capy::mutable_buffer(b1, b1_len));
                    re1 = ec; rn1 = n;
                }
                {
                    auto [ec, n] = co_await receiver.recv(
                        capy::mutable_buffer(b2, b2_len));
                    re2 = ec; rn2 = n;
                }
                d = true;
            }(s1, s2,
              msg1, std::strlen(msg1), msg2, std::strlen(msg2),
              buf1, sizeof(buf1), buf2, sizeof(buf2),
              send_ec1, sent1, send_ec2, sent2,
              recv_ec1, recvd1, recv_ec2, recvd2, done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(done, true);
        BOOST_TEST_EQ(!send_ec1, true);
        BOOST_TEST_EQ(!send_ec2, true);
        BOOST_TEST_EQ(!recv_ec1, true);
        BOOST_TEST_EQ(!recv_ec2, true);

        // Each recv returns exactly one datagram -- not a merged stream
        BOOST_TEST_EQ(recvd1, std::strlen(msg1));
        BOOST_TEST_EQ(recvd2, std::strlen(msg2));
        BOOST_TEST_EQ(std::string(buf1, recvd1), std::string(msg1));
        BOOST_TEST_EQ(std::string(buf2, recvd2), std::string(msg2));
    }

#ifdef __linux__
    void testAbstractSocket()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        // Abstract socket: null byte prefix, no filesystem entry. The
        // abstract namespace is global per network namespace, so the
        // .epoll/.select/.io_uring variants -- separate processes run
        // concurrently by `ctest --parallel` -- must not share a name, or
        // they race to bind it and the loser gets EADDRINUSE. The pid makes
        // each process's names unique; abstract names are released on close,
        // so a reused pid from an exited run cannot clash.
        auto const tag =
            "corosio_test_abstract_dgram_" + std::to_string(::getpid());
        std::string abs_path1(1, '\0');
        abs_path1 += tag + "_1";
        std::string abs_path2(1, '\0');
        abs_path2 += tag + "_2";

        local_datagram_socket s1(ioc);
        local_datagram_socket s2(ioc);
        s1.open();
        s2.open();

        auto ec1 = s1.bind(local_endpoint(abs_path1));
        auto ec2 = s2.bind(local_endpoint(abs_path2));
        BOOST_TEST_EQ(!ec1, true);
        BOOST_TEST_EQ(!ec2, true);

        char const msg[] = "abstract dgram";
        char buf[64]     = {};
        std::error_code send_ec, recv_ec;
        std::size_t sent = 0, recvd = 0;
        bool send_done = false, recv_done = false;
        local_endpoint source;

        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* data, std::size_t len,
               local_endpoint dest,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.send_to(
                    capy::const_buffer(data, len), dest);
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s1, msg, std::strlen(msg), local_endpoint(abs_path2),
              send_ec, sent, send_done));

        capy::run_async(ex)(
            [](local_datagram_socket& s, char* data, std::size_t len,
               local_endpoint& source_out,
               std::error_code& ec_out, std::size_t& n_out,
               bool& done) -> capy::task<> {
                auto [ec, n] = co_await s.recv_from(
                    capy::mutable_buffer(data, len), source_out);
                ec_out = ec;
                n_out  = n;
                done   = true;
            }(s2, buf, sizeof(buf), source, recv_ec, recvd, recv_done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(send_done, true);
        BOOST_TEST_EQ(!send_ec, true);
        BOOST_TEST_EQ(recv_done, true);
        BOOST_TEST_EQ(!recv_ec, true);
        BOOST_TEST_EQ(recvd, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf, recvd), std::string(msg));

        // Source should be the sender's abstract path
        BOOST_TEST_EQ(source.path(), abs_path1);
        BOOST_TEST_EQ(source.is_abstract(), true);
    }
#endif // __linux__

    void testRecvPeek()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");
        auto ex = ioc.get_executor();

        // Send a message, peek at it, then consume it.
        // Peek should not remove the message from the queue.
        char const msg[] = "peek test";
        char buf1[64] = {};
        char buf2[64] = {};
        std::error_code se, re1, re2;
        std::size_t sn = 0, rn1 = 0, rn2 = 0;
        bool done = false;

        capy::run_async(ex)(
            [](local_datagram_socket& sender,
               local_datagram_socket& receiver,
               char const* data, std::size_t len,
               char* b1, std::size_t b1_len,
               char* b2, std::size_t b2_len,
               std::error_code& se_out, std::size_t& sn_out,
               std::error_code& re1_out, std::size_t& rn1_out,
               std::error_code& re2_out, std::size_t& rn2_out,
               bool& d) -> capy::task<> {
                {
                    auto [ec, n] = co_await sender.send(
                        capy::const_buffer(data, len));
                    se_out = ec; sn_out = n;
                }
                // Peek -- should not consume
                {
                    auto [ec, n] = co_await receiver.recv(
                        capy::mutable_buffer(b1, b1_len),
                        message_flags::peek);
                    re1_out = ec; rn1_out = n;
                }
                // Normal recv -- should get same data
                {
                    auto [ec, n] = co_await receiver.recv(
                        capy::mutable_buffer(b2, b2_len));
                    re2_out = ec; rn2_out = n;
                }
                d = true;
            }(s1, s2, msg, std::strlen(msg),
              buf1, sizeof(buf1), buf2, sizeof(buf2),
              se, sn, re1, rn1, re2, rn2, done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(done, true);
        BOOST_TEST_EQ(!se, true);
        BOOST_TEST_EQ(!re1, true);
        BOOST_TEST_EQ(!re2, true);

        // Both reads should return the same data
        BOOST_TEST_EQ(rn1, std::strlen(msg));
        BOOST_TEST_EQ(rn2, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf1, rn1), std::string(msg));
        BOOST_TEST_EQ(std::string(buf2, rn2), std::string(msg));
    }

    void testRecvFromPeek()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;
        auto path1 = tmp1.path();
        auto path2 = tmp2.path();

        local_datagram_socket s1(ioc);
        local_datagram_socket s2(ioc);
        s1.open();
        s2.open();

        auto ec1 = s1.bind(local_endpoint(path1));
        auto ec2 = s2.bind(local_endpoint(path2));
        BOOST_TEST_EQ(!ec1, true);
        BOOST_TEST_EQ(!ec2, true);

        // Send a message via send_to, peek with recv_from, then
        // consume with recv_from. Exercises the connectionless
        // recv_from path with message_flags::peek.
        char const msg[] = "recv_from peek";
        char buf1[64] = {};
        char buf2[64] = {};
        std::error_code se, re1, re2;
        std::size_t sn = 0, rn1 = 0, rn2 = 0;
        local_endpoint src1, src2;
        bool done = false;

        capy::run_async(ex)(
            [](local_datagram_socket& sender,
               local_datagram_socket& receiver,
               char const* data, std::size_t len,
               local_endpoint dest,
               char* b1, std::size_t b1_len,
               char* b2, std::size_t b2_len,
               std::error_code& se_out, std::size_t& sn_out,
               std::error_code& re1_out, std::size_t& rn1_out,
               local_endpoint& src1_out,
               std::error_code& re2_out, std::size_t& rn2_out,
               local_endpoint& src2_out,
               bool& d) -> capy::task<> {
                {
                    auto [ec, n] = co_await sender.send_to(
                        capy::const_buffer(data, len), dest);
                    se_out = ec; sn_out = n;
                }
                // Peek via recv_from -- should not consume
                {
                    auto [ec, n] = co_await receiver.recv_from(
                        capy::mutable_buffer(b1, b1_len),
                        src1_out,
                        message_flags::peek);
                    re1_out = ec; rn1_out = n;
                }
                // Normal recv_from -- should get same data
                {
                    auto [ec, n] = co_await receiver.recv_from(
                        capy::mutable_buffer(b2, b2_len),
                        src2_out);
                    re2_out = ec; rn2_out = n;
                }
                d = true;
            }(s1, s2, msg, std::strlen(msg), local_endpoint(path2),
              buf1, sizeof(buf1), buf2, sizeof(buf2),
              se, sn, re1, rn1, src1, re2, rn2, src2, done));

        ioc.run();
        ioc.restart();

        BOOST_TEST_EQ(done, true);
        BOOST_TEST_EQ(!se, true);
        BOOST_TEST_EQ(!re1, true);
        BOOST_TEST_EQ(!re2, true);

        // Both reads should return the same data
        BOOST_TEST_EQ(rn1, std::strlen(msg));
        BOOST_TEST_EQ(rn2, std::strlen(msg));
        BOOST_TEST_EQ(std::string(buf1, rn1), std::string(msg));
        BOOST_TEST_EQ(std::string(buf2, rn2), std::string(msg));

        // Source should be the sender's bound path
        BOOST_TEST_EQ(src1.path(), path1);
        BOOST_TEST_EQ(src2.path(), path1);
    }

    void testMoveAssign()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc);
        local_datagram_socket s2(ioc);
        s1.open();
        BOOST_TEST_EQ(s1.is_open(), true);

        s2 = std::move(s1);
        BOOST_TEST_EQ(s2.is_open(), true);
        BOOST_TEST_EQ(s1.is_open(), false);
    }

    void testCancelOnClosed()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        // cancel() on a closed socket is a no-op (early return).
        sock.cancel();
        BOOST_TEST_EQ(sock.is_open(), false);
    }

    void testNativeHandleClosed()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        BOOST_TEST_EQ(sock.native_handle() < 0, true);

        sock.open();
        BOOST_TEST(sock.native_handle() >= 0);
    }

    void testEndpointsClosed()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        BOOST_TEST_EQ(sock.local_endpoint().empty(), true);
        BOOST_TEST_EQ(sock.remote_endpoint().empty(), true);
    }

    void testEndpointsBound()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        sock.open();

        test::temp_socket_dir tmp;
        auto path = tmp.path();
        auto ec   = sock.bind(local_endpoint(path));
        BOOST_TEST_EQ(!ec, true);

        BOOST_TEST_EQ(sock.local_endpoint().path(), path);
        BOOST_TEST_EQ(sock.remote_endpoint().empty(), true);
    }

    void testShutdown()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        // Throwing overload (best-effort, may report ENOTCONN).
        s1.shutdown(shutdown_send);

        // Non-throwing overload
        std::error_code ec;
        s2.shutdown(shutdown_send, ec);

        // Closed-socket no-ops
        local_datagram_socket closed(ioc);
        closed.shutdown(shutdown_send);

        std::error_code ec2;
        closed.shutdown(shutdown_send, ec2);
        BOOST_TEST_EQ(!ec2, true);
    }

    void testBindClosedThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        // NOLINTNEXTLINE(bugprone-unused-return-value)
        BOOST_TEST_THROWS(sock.bind(local_endpoint("/tmp/never")),
                          std::logic_error);
    }

    void testReleaseClosedThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.release();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testAvailableClosedThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);

        bool caught = false;
        try
        {
            (void)sock.available();
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testAvailable()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        // Send a datagram, then check available on the receive side
        auto ex = ioc.get_executor();
        char const msg[] = "hello";
        bool done = false;
        capy::run_async(ex)(
            [](local_datagram_socket& s, char const* m, std::size_t n,
               bool& d) -> capy::task<> {
                (void)co_await s.send(capy::const_buffer(m, n));
                d = true;
            }(s1, msg, std::strlen(msg), done));

        ioc.run();
        ioc.restart();

        BOOST_TEST(done);
        BOOST_TEST(s2.available() >= std::strlen(msg));
    }

    void testAssignAlreadyOpenThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        sock.open();

        bool caught = false;
        try
        {
            sock.assign(-1);
        }
        catch (std::logic_error const&)
        {
            caught = true;
        }
        BOOST_TEST(caught);
    }

    void testRelease()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");
        (void)s2;
        BOOST_TEST(s1.is_open());

        int fd = s1.release();
        BOOST_TEST(fd >= 0);
        BOOST_TEST_EQ(s1.is_open(), false);
        ::close(fd);
    }

    void testSendOnClosedThrows()
    {
        io_context ioc(Backend);
        local_datagram_socket sock(ioc);
        char const m[] = "x";

        bool caught_send = false;
        try
        {
            (void)sock.send(capy::const_buffer(m, 1));
        }
        catch (std::logic_error const&)
        {
            caught_send = true;
        }
        BOOST_TEST(caught_send);

        bool caught_send_to = false;
        try
        {
            (void)sock.send_to(
                capy::const_buffer(m, 1), local_endpoint("/tmp/x"));
        }
        catch (std::logic_error const&)
        {
            caught_send_to = true;
        }
        BOOST_TEST(caught_send_to);

        char buf[1];
        bool caught_recv = false;
        try
        {
            (void)sock.recv(capy::mutable_buffer(buf, 1));
        }
        catch (std::logic_error const&)
        {
            caught_recv = true;
        }
        BOOST_TEST(caught_recv);

        local_endpoint src;
        bool caught_recv_from = false;
        try
        {
            (void)sock.recv_from(capy::mutable_buffer(buf, 1), src);
        }
        catch (std::logic_error const&)
        {
            caught_recv_from = true;
        }
        BOOST_TEST(caught_recv_from);
    }

    void testCancelPendingRecv()
    {
        io_context ioc(Backend);
        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");
        (void)s1;

        auto ex = ioc.get_executor();
        std::error_code recv_ec;
        bool recv_done = false;

        capy::run_async(ex)(
            [](local_datagram_socket& s,
               std::error_code& ec_out, bool& done) -> capy::task<> {
                char buf[8];
                auto [ec, n] = co_await s.recv(
                    capy::mutable_buffer(buf, sizeof(buf)));
                (void)n;
                ec_out = ec;
                done   = true;
            }(s2, recv_ec, recv_done));

        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            s2.cancel();
        };
        capy::run_async(ex)(canceller());

        ioc.run();

        BOOST_TEST(recv_done);
        BOOST_TEST(recv_ec == capy::cond::canceled);
    }

    void run()
    {
        testConstruction();
        testOpen();
        testMove();
        testMoveAssign();
        testCancelOnClosed();
        testNativeHandleClosed();
        testEndpointsClosed();
        testEndpointsBound();
        testShutdown();
        testBindClosedThrows();
        testReleaseClosedThrows();
        testAvailableClosedThrows();
        testAvailable();
        testAssignAlreadyOpenThrows();
        testRelease();
        testSendOnClosedThrows();
        testCancelPendingRecv();
        testSendRecvConnected();
        testExplicitBind();
        testSendToRecvFrom();
        testBindFailure();
        testDatagramBoundary();
        testRecvPeek();
        testRecvFromPeek();
#ifdef __linux__
        testAbstractSocket();
#endif
    }
};

COROSIO_BACKEND_TESTS(
    local_datagram_socket_test, "boost.corosio.local_datagram_socket")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
