//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Coverage tests for datagram deferred (would-block) paths and per-op
// stop-token cancellation. The per-type unit tests send before receiving,
// so datagram ops complete on the speculative fast path; these tests park
// the op first so the reactor registration, deferred perform_io
// completion, and cancel_single_op slot lookups all execute.

#include <boost/corosio/detail/platform.hpp>

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/corosio/udp.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/wait_type.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <cstring>
#include <stop_token>
#include <string_view>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>

#include <boost/corosio/test/temp_path.hpp>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct datagram_paths_test
{
    // Connected UDP pair; both sockets bound to ephemeral loopback ports.
    static void make_udp_pair(io_context& ioc, udp_socket& a, udp_socket& b)
    {
        a.open(udp::v4());
        b.open(udp::v4());
        auto ec1 = a.bind(endpoint(ipv4_address::loopback(), 0));
        auto ec2 = b.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);

        bool done = false;
        auto connector = [&]() -> capy::task<> {
            auto [ca] = co_await a.connect(b.local_endpoint());
            auto [cb] = co_await b.connect(a.local_endpoint());
            BOOST_TEST(!ca);
            BOOST_TEST(!cb);
            done = true;
        };
        capy::run_async(ioc.get_executor())(connector());
        ioc.run();
        ioc.restart();
        BOOST_TEST(done);
    }

    // Park a connected recv() before any datagram exists, then send.
    // Exercises the EAGAIN -> register_op path and the deferred
    // reactor_recv_op::perform_io completion.
    void testDeferredRecvUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket s1(ioc), s2(ioc);
        make_udp_pair(ioc, s1, s2);

        constexpr std::string_view payload = "deferred recv";
        char buf[64] = {};
        std::error_code recv_ec, send_ec;
        std::size_t recv_n = 0;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(
                capy::mutable_buffer(buf, sizeof(buf)));
            recv_ec = ec;
            recv_n  = n;
        };
        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.send(
                capy::const_buffer(payload.data(), payload.size()));
            send_ec = ec;
            (void)n;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(!send_ec);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(recv_n, payload.size());
        BOOST_TEST_EQ(std::string_view(buf, recv_n), payload);
    }

    // Same as above through the unconnected recv_from()/send_to() pair.
    void testDeferredRecvFromUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv_sock(ioc), send_sock(ioc);
        recv_sock.open(udp::v4());
        send_sock.open(udp::v4());
        auto ec1 = recv_sock.bind(endpoint(ipv4_address::loopback(), 0));
        auto ec2 = send_sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);

        constexpr std::string_view payload = "deferred recv_from";
        char buf[64] = {};
        std::error_code recv_ec, send_ec;
        std::size_t recv_n = 0;
        endpoint source;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await recv_sock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            recv_ec = ec;
            recv_n  = n;
        };
        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await send_sock.send_to(
                capy::const_buffer(payload.data(), payload.size()),
                recv_sock.local_endpoint());
            send_ec = ec;
            (void)n;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(!send_ec);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(recv_n, payload.size());
        BOOST_TEST_EQ(source.port(), send_sock.local_endpoint().port());
    }

    // Stop-token cancel of a parked wait(read). Routes through the op's
    // on_cancel -> cancel_single_op -> op_to_desc_slot wait_rd_ entry.
    void testStopTokenCancelWaitReadUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::stop_source ss;
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
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // Stop-token cancel of a parked wait(error); never becomes ready on
    // a healthy socket, so cancellation is the only way out.
    void testStopTokenCancelWaitErrorUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket sock(ioc);
        sock.open(udp::v4());
        auto bec = sock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        std::stop_source ss;
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
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

#if BOOST_COROSIO_POSIX

    void testDeferredRecvLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        constexpr std::string_view payload = "deferred local recv";
        char buf[64] = {};
        std::error_code recv_ec, send_ec;
        std::size_t recv_n = 0;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(
                capy::mutable_buffer(buf, sizeof(buf)));
            recv_ec = ec;
            recv_n  = n;
        };
        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.send(
                capy::const_buffer(payload.data(), payload.size()));
            send_ec = ec;
            (void)n;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(!send_ec);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(std::string_view(buf, recv_n), payload);
    }

    void testDeferredRecvFromLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;

        local_datagram_socket s1(ioc), s2(ioc);
        s1.open();
        s2.open();
        auto ec1 = s1.bind(local_endpoint(tmp1.path()));
        auto ec2 = s2.bind(local_endpoint(tmp2.path()));
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);

        constexpr std::string_view payload = "deferred local recv_from";
        char buf[64] = {};
        std::error_code recv_ec, send_ec;
        std::size_t recv_n = 0;
        local_endpoint source;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            recv_ec = ec;
            recv_n  = n;
        };
        auto sender = [&]() -> capy::task<> {
            auto [ec, n] = co_await s2.send_to(
                capy::const_buffer(payload.data(), payload.size()),
                local_endpoint(tmp1.path()));
            send_ec = ec;
            (void)n;
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(!send_ec);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(std::string_view(buf, recv_n), payload);
    }

    // Burst-send 1 KiB datagrams into shrunken kernel buffers with the
    // reader delayed. On Linux a send into a full peer buffer returns
    // EAGAIN, parking the op until the reader drains the queue; BSD
    // and macOS report ENOBUFS, completing the send with an error.
    void testDeferredSendLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        s1.set_option(socket_option::send_buffer_size(2048));
        s2.set_option(socket_option::receive_buffer_size(2048));

        constexpr std::size_t count = 32;
        char dgram[1024];
        std::memset(dgram, 'D', sizeof(dgram));

        std::error_code send_ec, recv_ec;
        std::size_t sent = 0, received = 0;
        bool writer_done = false;

        auto writer = [&]() -> capy::task<> {
            while (sent < count)
            {
                auto [ec, n] = co_await s1.send(
                    capy::const_buffer(dgram, sizeof(dgram)));
                if (ec)
                {
                    send_ec = ec;
                    break;
                }
                BOOST_TEST_EQ(n, sizeof(dgram));
                ++sent;
            }
            writer_done = true;
        };
        auto reader = [&]() -> capy::task<> {
            // Let the writer fill the kernel queue and park first.
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();

            char buf[2048];
            while (!writer_done || received < sent)
            {
                auto [ec, n] = co_await s2.recv(
                    capy::mutable_buffer(buf, sizeof(buf)));
                if (ec)
                {
                    recv_ec = ec;
                    co_return;
                }
                BOOST_TEST_EQ(n, sizeof(dgram));
                ++received;
            }
        };

        capy::run_async(ex)(writer());
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(writer_done);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(received, sent);
#if defined(__linux__)
        BOOST_TEST(!send_ec);
        BOOST_TEST_EQ(sent, count);
#else
        // Sends that completed were delivered; the rest reported the
        // full queue. Exact behavior varies by kernel, so a clean run
        // is accepted alongside ENOBUFS.
        BOOST_TEST(!send_ec || send_ec == std::errc::no_buffer_space);
#endif
    }

    // Same buffer-pressure pattern through send_to()/recv_from().
    void testDeferredSendToLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;

        local_datagram_socket s1(ioc), s2(ioc);
        s1.open();
        s2.open();
        auto ec1 = s1.bind(local_endpoint(tmp1.path()));
        auto ec2 = s2.bind(local_endpoint(tmp2.path()));
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);

        s1.set_option(socket_option::send_buffer_size(2048));
        s2.set_option(socket_option::receive_buffer_size(2048));

        constexpr std::size_t count = 32;
        char dgram[1024];
        std::memset(dgram, 'T', sizeof(dgram));

        std::error_code send_ec, recv_ec;
        std::size_t sent = 0, received = 0;
        local_endpoint dest(tmp2.path());

        bool writer_done = false;

        auto writer = [&]() -> capy::task<> {
            while (sent < count)
            {
                auto [ec, n] = co_await s1.send_to(
                    capy::const_buffer(dgram, sizeof(dgram)), dest);
                if (ec)
                {
                    send_ec = ec;
                    break;
                }
                BOOST_TEST_EQ(n, sizeof(dgram));
                ++sent;
            }
            writer_done = true;
        };
        auto reader = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();

            char buf[2048];
            local_endpoint source;
            while (!writer_done || received < sent)
            {
                auto [ec, n] = co_await s2.recv_from(
                    capy::mutable_buffer(buf, sizeof(buf)), source);
                if (ec)
                {
                    recv_ec = ec;
                    co_return;
                }
                BOOST_TEST_EQ(n, sizeof(dgram));
                ++received;
            }
        };

        capy::run_async(ex)(writer());
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(writer_done);
        BOOST_TEST(!recv_ec);
        BOOST_TEST_EQ(received, sent);
#if defined(__linux__)
        BOOST_TEST(!send_ec);
        BOOST_TEST_EQ(sent, count);
#else
        // Sends that completed were delivered; the rest reported the
        // full queue. Exact behavior varies by kernel, so a clean run
        // is accepted alongside ENOBUFS.
        BOOST_TEST(!send_ec || send_ec == std::errc::no_buffer_space);
#endif
    }

    // Stop-token cancel of a parked connected recv on a local socket.
    void testStopTokenCancelRecvLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        std::stop_source ss;
        std::error_code recv_ec;
        bool recv_done = false;
        char buf[64];

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await s1.recv(
                capy::mutable_buffer(buf, sizeof(buf)));
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

    // Stop-token cancel of a parked recv_from on a bound local socket.
    void testStopTokenCancelRecvFromLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp;

        local_datagram_socket sock(ioc);
        sock.open();
        auto bec = sock.bind(local_endpoint(tmp.path()));
        BOOST_TEST(!bec);

        std::stop_source ss;
        std::error_code recv_ec;
        bool recv_done = false;
        char buf[64];
        local_endpoint source;

        auto receiver = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
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

    // Fill the kernel queue with no reader, then stop-token cancel the
    // in-flight send. On Linux the send parks (covering the send_wr_
    // slot lookup) and cancels; BSD and macOS complete it with ENOBUFS
    // before the cancel lands.
    void testStopTokenCancelSendLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

        s1.set_option(socket_option::send_buffer_size(2048));
        s2.set_option(socket_option::receive_buffer_size(2048));

        char dgram[1024];
        std::memset(dgram, 'C', sizeof(dgram));

        std::stop_source ss;
        std::error_code send_ec;
        bool send_done = false;

        auto writer = [&]() -> capy::task<> {
            // 64 KiB into ~4 KiB of queue: parks long before the limit.
            for (int i = 0; i < 64; ++i)
            {
                auto [ec, n] = co_await s1.send(
                    capy::const_buffer(dgram, sizeof(dgram)));
                (void)n;
                if (ec)
                {
                    send_ec   = ec;
                    send_done = true;
                    co_return;
                }
            }
            send_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(send_done);
#if defined(__linux__)
        BOOST_TEST(send_ec == capy::cond::canceled);
#else
        // ENOBUFS preempts the cancellation unless the kernel parked
        // the send after all.
        BOOST_TEST(send_ec == capy::cond::canceled
            || send_ec == std::errc::no_buffer_space);
#endif
    }

    // Same as above through send_to(); covers the wr_ slot lookup on
    // Linux.
    void testStopTokenCancelSendToLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        test::temp_socket_dir tmp1;
        test::temp_socket_dir tmp2;

        local_datagram_socket s1(ioc), s2(ioc);
        s1.open();
        s2.open();
        auto ec1 = s1.bind(local_endpoint(tmp1.path()));
        auto ec2 = s2.bind(local_endpoint(tmp2.path()));
        BOOST_TEST(!ec1);
        BOOST_TEST(!ec2);

        s1.set_option(socket_option::send_buffer_size(2048));
        s2.set_option(socket_option::receive_buffer_size(2048));

        char dgram[1024];
        std::memset(dgram, 'C', sizeof(dgram));

        std::stop_source ss;
        std::error_code send_ec;
        bool send_done = false;
        local_endpoint dest(tmp2.path());

        auto writer = [&]() -> capy::task<> {
            for (int i = 0; i < 64; ++i)
            {
                auto [ec, n] = co_await s1.send_to(
                    capy::const_buffer(dgram, sizeof(dgram)), dest);
                (void)n;
                if (ec)
                {
                    send_ec   = ec;
                    send_done = true;
                    co_return;
                }
            }
            send_done = true;
        };
        auto canceller = [&]() -> capy::task<> {
            timer t(ioc);
            t.expires_after(std::chrono::milliseconds(20));
            (void)co_await t.wait();
            ss.request_stop();
        };

        capy::run_async(ex, ss.get_token())(writer());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(send_done);
#if defined(__linux__)
        BOOST_TEST(send_ec == capy::cond::canceled);
#else
        // ENOBUFS preempts the cancellation unless the kernel parked
        // the send after all.
        BOOST_TEST(send_ec == capy::cond::canceled
            || send_ec == std::errc::no_buffer_space);
#endif
    }

    void testStopTokenCancelWaitReadLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

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

    void testStopTokenCancelWaitErrorLocal()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        local_datagram_socket s1(ioc), s2(ioc);
        if (auto ec = connect_pair(s1, s2))
            throw std::system_error(ec, "connect_pair");

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

#endif // BOOST_COROSIO_POSIX

    void run()
    {
        testDeferredRecvUdp();
        testDeferredRecvFromUdp();
        testStopTokenCancelWaitReadUdp();
        testStopTokenCancelWaitErrorUdp();
#if BOOST_COROSIO_POSIX
        testDeferredRecvLocal();
        testDeferredRecvFromLocal();
        testDeferredSendLocal();
        testDeferredSendToLocal();
        testStopTokenCancelRecvLocal();
        testStopTokenCancelRecvFromLocal();
        testStopTokenCancelSendLocal();
        testStopTokenCancelSendToLocal();
        testStopTokenCancelWaitReadLocal();
        testStopTokenCancelWaitErrorLocal();
#endif
    }
};

COROSIO_BACKEND_TESTS(datagram_paths_test, "boost.corosio.datagram_paths")

} // namespace boost::corosio
