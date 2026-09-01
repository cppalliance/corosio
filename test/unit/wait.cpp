//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header is self-contained.
#include <boost/corosio/wait_type.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/test/temp_path.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <tuple>

#if BOOST_COROSIO_POSIX
// Raw descriptor access for the write-backpressure tests.
#include <poll.h>
#include <sys/socket.h>
#else
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <ws2tcpip.h>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {
namespace {

/// Return true if the descriptor currently accepts a non-blocking write.
bool
socket_writable(native_handle_type fd) noexcept
{
#if BOOST_COROSIO_POSIX
    ::pollfd pfd{};
    pfd.fd     = static_cast<int>(fd);
    pfd.events = POLLOUT;
    return ::poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT) != 0;
#else
    WSAPOLLFD pfd{};
    pfd.fd     = static_cast<SOCKET>(fd);
    pfd.events = POLLWRNORM;
    return ::WSAPoll(&pfd, 1, 0) == 1 && (pfd.revents & POLLWRNORM) != 0;
#endif
}

/// Shrink a socket buffer so the peer's window closes after a few writes.
void
shrink_socket_buffer(native_handle_type fd, int optname) noexcept
{
    int size = 4096;
#if BOOST_COROSIO_POSIX
    ::setsockopt(
        static_cast<int>(fd), SOL_SOCKET, optname, &size, sizeof(size));
#else
    ::setsockopt(
        static_cast<SOCKET>(fd), SOL_SOCKET, optname,
        reinterpret_cast<char const*>(&size), sizeof(size));
#endif
}

/** Fill the descriptor's send buffer until the peer's window closes.

    A single refusal is not proof of a stall: bytes still in flight
    can free space again without the peer reading anything, which
    would let a write wait complete with no drain. Keep writing until
    a poll probe agrees the socket is unwritable.

    @param fd The descriptor to fill.

    @return The number of bytes accepted, or zero if the platform
    kept accepting past the cap or never settled into a stall.
*/
std::size_t
fill_send_buffer(native_handle_type fd)
{
    constexpr std::size_t cap      = 1u << 22;
    constexpr int         spin_max = 1000;

    char        blob[4096] = {};
    std::size_t filled     = 0;
    int         spins      = 0;

#if !BOOST_COROSIO_POSIX
    // Overlapped sockets are blocking by default; a full send buffer
    // would stall the test thread instead of refusing the write.
    u_long nonblocking = 1;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonblocking);
#endif

    for (;;)
    {
#if BOOST_COROSIO_POSIX
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
        constexpr int flags = MSG_DONTWAIT;
#endif
        auto n = ::send(static_cast<int>(fd), blob, sizeof(blob), flags);
        bool refused = n < 0;
        if (refused)
        {
            BOOST_TEST(errno == EAGAIN || errno == EWOULDBLOCK);
        }
#else
        auto n = ::send(
            static_cast<SOCKET>(fd), blob, static_cast<int>(sizeof(blob)), 0);
        bool refused = n == SOCKET_ERROR;
        if (refused)
        {
            BOOST_TEST(::WSAGetLastError() == WSAEWOULDBLOCK);
        }
#endif
        if (refused)
        {
            if (!socket_writable(fd))
                break;
            if (++spins > spin_max)
            {
                // Refusals the poll keeps disagreeing with never
                // establish a stall, so the caller has nothing to
                // test: report no fill and let it skip.
                filled = 0;
                break;
            }
            continue;
        }
        spins = 0;
        filled += static_cast<std::size_t>(n);
        if (filled > cap)
        {
            filled = 0;
            break;
        }
    }

#if !BOOST_COROSIO_POSIX
    nonblocking = 0;
    ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonblocking);
#endif
    return filled;
}

/** Create a connected pair whose socket buffers are pinned small.

    The receive window is negotiated during the handshake, and buffer
    options set on an established socket no longer bind it: Darwin in
    particular keeps growing the receive side, so a "full" send buffer
    drains again without the peer reading anything. Shrinking the
    listener and the client before the handshake makes the
    backpressure real on every platform.
*/
std::pair<tcp_socket, tcp_socket>
make_backpressured_pair(io_context& ioc)
{
    auto ex = ioc.get_executor();

    std::error_code accept_ec;
    std::error_code connect_ec;
    bool accept_done  = false;
    bool connect_done = false;

    tcp_acceptor acc(ioc);
    BOOST_TEST(!acc.open());
    acc.set_option(socket_option::reuse_address(true));
    shrink_socket_buffer(acc.native_handle(), SO_SNDBUF);
    shrink_socket_buffer(acc.native_handle(), SO_RCVBUF);
    auto bec = acc.bind(endpoint(ipv4_address::loopback(), 0));
    BOOST_TEST(!bec);
    auto lec = acc.listen();
    BOOST_TEST(!lec);
    auto port = acc.local_endpoint().port();

    tcp_socket s1(ioc);
    tcp_socket s2(ioc);
    BOOST_TEST(!s2.open());
    shrink_socket_buffer(s2.native_handle(), SO_SNDBUF);
    shrink_socket_buffer(s2.native_handle(), SO_RCVBUF);

    auto acceptor_task = [&]() -> capy::task<> {
        auto [ec] = co_await acc.accept(s1);
        accept_ec   = ec;
        accept_done = true;
    };
    auto connect_task = [&]() -> capy::task<> {
        auto [ec] =
            co_await s2.connect(endpoint(ipv4_address::loopback(), port));
        connect_ec   = ec;
        connect_done = true;
    };
    capy::run_async(ex)(acceptor_task());
    capy::run_async(ex)(connect_task());
    ioc.run();
    ioc.restart();

    BOOST_TEST(accept_done && !accept_ec);
    BOOST_TEST(connect_done && !connect_ec);
    return {std::move(s1), std::move(s2)};
}

} // namespace

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
            [[maybe_unused]] auto [ec, n] = co_await s2.write_some(
                capy::const_buffer(payload.data(), payload.size()));
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, payload.size());
    }

    // A freshly connected socket probes writable, so wait_write
    // completes without waiting for anything to drain.
    void testWaitWriteReady()
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

    // wait_write must park while the send buffer is full: completing
    // immediately turns an external "retry when writable" flush loop
    // into a busy spin exactly when the socket is backpressured.
    void testWaitWriteParksUntilDrained()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = make_backpressured_pair(ioc);

        auto filled = fill_send_buffer(s1.native_handle());
        if (filled == 0)
            return; // the platform refuses to backpressure

        std::error_code wait_ec;
        bool wait_done = false;
        bool drained   = false;

        auto waiter = [&]() -> capy::task<> {
            for (;;)
            {
                auto [ec] = co_await s1.wait(wait_type::write);
                wait_ec   = ec;
                wait_done = true;
                if (ec)
                    break;
                // The contract under test: a completed write wait
                // means a non-blocking write can make progress right
                // now. Checked on every wake, because the kernel may
                // free send space on its own (window probes, buffer
                // compaction) — such a wake is a true writability
                // report, just not the drain we sequenced. Re-wait
                // until the peer has actually taken bytes.
                BOOST_TEST(socket_writable(s1.native_handle()));
                if (drained)
                    break;
            }
        };
        auto drainer = [&]() -> capy::task<> {
            std::array<char, 8192> sink{};
            std::size_t got = 0;
            while (got < filled)
            {
                auto [ec, n] = co_await s2.read_some(
                    capy::mutable_buffer(sink.data(), sink.size()));
                if (ec)
                    break;
                got += n;
                drained = true;
            }
        };

        // Spawn order is park order: the wait must be outstanding
        // before the drain reopens the window.
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(drainer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(drained);
    }

    // Cancelling a parked write wait completes it as canceled instead
    // of leaving the op in the descriptor's write-wait slot.
    void testWaitWriteCancel()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = make_backpressured_pair(ioc);

        if (fill_send_buffer(s1.native_handle()) == 0)
            return; // the platform refuses to backpressure

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            wait_ec   = ec;
            wait_done = true;
        };
        // Spawn order is park order: the waiter's turn parks the wait
        // before the canceller's turn runs, and the cancel lands one
        // scheduler iteration later — no window for a kernel that
        // frees send-buffer space on its own.
        auto canceller = [&]() -> capy::task<> {
            s1.cancel();
            co_return;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // A cancel that reaches a wait outside its parked window must not
    // leak into the next wait in the same direction: the second wait
    // runs under a fresh token and must park until the peer drains.
    void testWaitWriteCancelDoesNotLeak()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = make_backpressured_pair(ioc);

        // First wait: writable socket, pre-stopped token. The stop
        // callback fires during initiation, before the op is in any
        // descriptor slot, and the wait completes canceled.
        std::stop_source ss;
        ss.request_stop();

        [[maybe_unused]] std::error_code first_ec;
        bool first_done = false;
        auto first = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::write);
            first_ec   = ec;
            first_done = true;
        };
        capy::run_async(ex, ss.get_token())(first());
        ioc.run();
        ioc.restart();

        // Whether the first wait reports canceled or the probe's
        // success is immaterial here; the subject is what its cancel
        // left behind.
        BOOST_TEST(first_done);

        // Second wait: full buffer, fresh token. It must park and
        // complete on the drain — not absorb the first wait's cancel.
        auto filled = fill_send_buffer(s1.native_handle());
        if (filled == 0)
            return; // the platform refuses to backpressure

        std::error_code wait_ec;
        bool wait_done = false;
        bool drained   = false;

        auto waiter = [&]() -> capy::task<> {
            for (;;)
            {
                auto [ec] = co_await s1.wait(wait_type::write);
                wait_ec   = ec;
                wait_done = true;
                if (ec)
                    break;
                BOOST_TEST(socket_writable(s1.native_handle()));
                if (drained)
                    break;
            }
        };
        auto drainer = [&]() -> capy::task<> {
            std::array<char, 8192> sink{};
            std::size_t got = 0;
            while (got < filled)
            {
                auto [ec, n] = co_await s2.read_some(
                    capy::mutable_buffer(sink.data(), sink.size()));
                if (ec)
                    break;
                got += n;
                drained = true;
            }
        };
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(drainer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(drained);
    }

    // UDP wait_read fires when a datagram arrives.
    void testWaitOnUdp()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv(ioc);
        BOOST_TEST(!recv.open(udp::v4()));
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        udp_socket send(ioc);
        BOOST_TEST(!send.open(udp::v4()));

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await recv.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char dg[1] = { 'X' };
            [[maybe_unused]] auto [ec, n] = co_await send.send_to(
                capy::const_buffer(dg, sizeof(dg)),
                endpoint(ipv4_address::loopback(), port));
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Acceptor wait_read fires when a client connects; accept then succeeds.
    void testAcceptorWait()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        tcp_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
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
            [[maybe_unused]] auto [ec] = co_await client.connect(
                endpoint(ipv4_address::loopback(), port));
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(connector());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(peer.is_open());
    }

    // local_stream_socket wait_read fires when the peer writes.
    void testWaitOnLocalStream()
    {
        io_context ioc(Backend);
        auto ex   = ioc.get_executor();
        test::temp_socket_dir tmp;
        auto path = tmp.path();

        local_stream_acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        auto bec = acc.bind(local_endpoint(path));
        BOOST_TEST(!bec);
        auto lec = acc.listen();
        BOOST_TEST(!lec);

        local_stream_socket server(ioc);
        local_stream_socket client(ioc);
        BOOST_TEST(!client.open());

        auto accept_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await acc.accept(server);
        };
        auto connect_task = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec] = co_await client.connect(local_endpoint(path));
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
            [[maybe_unused]] auto [ec, n] = co_await client.write_some(
                capy::const_buffer(payload.data(), payload.size()));
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

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
            std::ignore = co_await delay(std::chrono::milliseconds(20));
            s1.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
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
        BOOST_TEST(!sock.open(udp::v4()));
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
            std::ignore = co_await delay(std::chrono::milliseconds(20));
            sock.cancel();
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // A short read that leaves bytes buffered consumes the readiness
    // edge; a subsequent wait_read must still complete because data
    // remains available.
    void testWaitReadAfterShortRead()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code read_ec;
        std::size_t bytes_read = 0;
        std::error_code wait_ec;
        bool wait_done = false;
        char first = 0;
        std::array<char, 8> rest{};
        std::size_t rest_n = 0;

        auto reader = [&]() -> capy::task<> {
            auto [ec1, n1] =
                co_await s1.read_some(capy::mutable_buffer(&first, 1));
            read_ec    = ec1;
            bytes_read = n1;
            auto [ec2] = co_await s1.wait(wait_type::read);
            wait_ec    = ec2;
            wait_done  = true;
            if (ec2)
                co_return;
            [[maybe_unused]] auto [ec3, n3] = co_await s1.read_some(
                capy::mutable_buffer(rest.data(), rest.size()));
            rest_n = n3;
        };
        auto writer = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec, n] = co_await s2.write_some(capy::const_buffer("xy", 2));
        };

        capy::run_async(ex)(reader());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(!read_ec);
        BOOST_TEST_EQ(bytes_read, 1u);
        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
        BOOST_TEST_EQ(first, 'x');
        BOOST_TEST_EQ(rest_n, 1u);
        BOOST_TEST_EQ(rest[0], 'y');
    }

    // Draining the socket with a speculative read leaves a cached
    // readiness edge behind; a subsequent wait_read must park on the
    // now-empty socket instead of completing on the stale edge. A
    // second socket pair sequences the cancel strictly after the wait
    // is parked.
    void testWaitReadParksAfterDrain()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::make_socket_pair(ioc);
        auto [t1, t2] = test::make_socket_pair(ioc);

        std::size_t drained_n = 0;
        std::error_code wait_ec;
        bool wait_done   = false;
        bool cancel_sent = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto driver = [&]() -> capy::task<> {
            // Latch a readiness edge on s1 with no read op parked...
            [[maybe_unused]] auto [wec, wn] = co_await s2.write_some(
                capy::const_buffer("xy", 2));
            // ...drain it, typically on the speculative success path...
            std::array<char, 8> buf{};
            [[maybe_unused]] auto [rec, rn] = co_await s1.read_some(
                capy::mutable_buffer(buf.data(), buf.size()));
            drained_n = rn;
            // ...then park the wait before the release signal exists.
            // Spawning here queues the wait initiation ahead of every
            // completion the "go" write can generate, so the cancel is
            // ordered after the park on FIFO schedulers and completion
            // ports alike.
            capy::run_async(ex)(waiter());
            [[maybe_unused]] auto [sec, sn] = co_await t2.write_some(
                capy::const_buffer("go", 2));
        };
        auto canceller = [&]() -> capy::task<> {
            char c[2];
            [[maybe_unused]] auto [ec, n] = co_await t1.read_some(
                capy::mutable_buffer(c, sizeof(c)));
            cancel_sent = true;
            s1.cancel();
        };

        capy::run_async(ex)(canceller());
        capy::run_async(ex)(driver());
        ioc.run();

        BOOST_TEST_EQ(drained_n, 2u);
        BOOST_TEST(cancel_sent);
        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // A parked recv consumes the readiness edge but only the first of
    // two buffered datagrams; a subsequent wait_read must complete
    // while the second datagram remains.
    void testWaitReadSecondDatagram()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket recv(ioc);
        BOOST_TEST(!recv.open(udp::v4()));
        auto bec = recv.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);
        auto port = recv.local_endpoint().port();

        udp_socket send(ioc);
        BOOST_TEST(!send.open(udp::v4()));

        std::size_t first_n = 0;
        std::error_code wait_ec;
        bool wait_done = false;

        auto receiver = [&]() -> capy::task<> {
            char dg[4];
            endpoint source;
            [[maybe_unused]] auto [ec, n] = co_await recv.recv_from(
                capy::mutable_buffer(dg, sizeof(dg)), source);
            first_n = n;
            auto [wec] = co_await recv.wait(wait_type::read);
            wait_ec   = wec;
            wait_done = true;
        };
        auto sender = [&]() -> capy::task<> {
            char a[1] = { 'a' };
            char b[1] = { 'b' };
            endpoint dst(ipv4_address::loopback(), port);
            [[maybe_unused]] auto [e1, n1] = co_await send.send_to(
                capy::const_buffer(a, sizeof(a)), dst);
            [[maybe_unused]] auto [e2, n2] = co_await send.send_to(
                capy::const_buffer(b, sizeof(b)), dst);
        };

        capy::run_async(ex)(receiver());
        capy::run_async(ex)(sender());
        ioc.run();

        BOOST_TEST_EQ(first_n, 1u);
        BOOST_TEST(wait_done);
        BOOST_TEST(!wait_ec);
    }

    // Draining the only buffered datagram on the speculative path
    // leaves a stale readiness edge; a subsequent wait_read must park
    // on the empty socket instead of completing on the stale edge.
    void testWaitUdpParksAfterDrain()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        udp_socket rsock(ioc);
        BOOST_TEST(!rsock.open(udp::v4()));
        auto bec = rsock.bind(endpoint(ipv4_address::loopback(), 0));
        BOOST_TEST(!bec);

        udp_socket ssock(ioc);
        BOOST_TEST(!ssock.open(udp::v4()));

        auto [t1, t2] = test::make_socket_pair(ioc);

        std::size_t drained_n = 0;
        std::error_code wait_ec;
        bool wait_done   = false;
        bool cancel_sent = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await rsock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };
        auto driver = [&]() -> capy::task<> {
            // Latch a readiness edge on rsock with no recv parked...
            char dg[1] = { 'x' };
            [[maybe_unused]] auto [wec, wn] = co_await ssock.send_to(
                capy::const_buffer(dg, sizeof(dg)),
                rsock.local_endpoint());
            // ...drain it, typically on the speculative success path...
            char buf[4];
            endpoint source;
            [[maybe_unused]] auto [rec, rn] = co_await rsock.recv_from(
                capy::mutable_buffer(buf, sizeof(buf)), source);
            drained_n = rn;
            // ...then park the wait before the release signal exists.
            // Spawning here queues the wait initiation ahead of every
            // completion the "go" write can generate, so the cancel is
            // ordered after the park on FIFO schedulers and completion
            // ports alike.
            capy::run_async(ex)(waiter());
            [[maybe_unused]] auto [sec, sn] = co_await t2.write_some(
                capy::const_buffer("go", 2));
        };
        auto canceller = [&]() -> capy::task<> {
            char c[2];
            [[maybe_unused]] auto [ec, n] = co_await t1.read_some(
                capy::mutable_buffer(c, sizeof(c)));
            cancel_sent = true;
            rsock.cancel();
        };

        capy::run_async(ex)(canceller());
        capy::run_async(ex)(driver());
        ioc.run();

        BOOST_TEST_EQ(drained_n, 1u);
        BOOST_TEST(cancel_sent);
        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    // wait(wait_type::error) on a listening acceptor parks in the
    // error-poll path (aux reactor on IOCP, POLL_ADD on io_uring);
    // cancel() completes it with the canceled condition.
    template<class Acceptor, class Endpoint>
    void checkAcceptorErrorWaitCancel(Endpoint ep)
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        Acceptor acc(ioc);
        BOOST_TEST(!acc.open());
        BOOST_TEST(!acc.bind(ep));
        BOOST_TEST(!acc.listen());

        std::error_code wait_ec;
        bool wait_done = false;
        capy::run_async(ex)(
            [](Acceptor& a, std::error_code& ec_out,
               bool& done) -> capy::task<> {
                auto [ec] = co_await a.wait(wait_type::error);
                ec_out = ec;
                done   = true;
            }(acc, wait_ec, wait_done));

        // Runs after the waiter has parked: coroutines start in
        // submission order on the single run thread.
        capy::run_async(ex)(
            [](Acceptor& a) -> capy::task<> {
                a.cancel();
                co_return;
            }(acc));

        ioc.run();
        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == capy::cond::canceled);
    }

    void testAcceptorErrorWaitCancel()
    {
        checkAcceptorErrorWaitCancel<tcp_acceptor>(
            endpoint(ipv4_address::loopback(), 0));
        test::temp_socket_dir tmp;
        checkAcceptorErrorWaitCancel<local_stream_acceptor>(
            local_endpoint(tmp.path()));
    }

    void run()
    {
        testWaitReadAndNoConsume();
        testWaitWriteReady();
        testWaitWriteParksUntilDrained();
        testWaitWriteCancel();
        testWaitWriteCancelDoesNotLeak();
        testAcceptorWait();
        testAcceptorErrorWaitCancel();
        testWaitOnLocalStream();
        testWaitOnUdp();
        testWaitReadAfterShortRead();
        testWaitReadParksAfterDrain();
        testWaitReadSecondDatagram();
        testWaitUdpParksAfterDrain();
        testCancellation();
        testUdpCancellation();
    }
};

COROSIO_BACKEND_TESTS(wait_test, "boost.corosio.wait")

// Closed-object contract: every backend completes wait/read/write on a
// closed socket with bad_file_descriptor instead of a platform code.
template<auto Backend>
struct wait_closed_test
{
    template<class Socket>
    void checkClosedWait()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        Socket sock(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await sock.wait(wait_type::read);
            wait_ec   = ec;
            wait_done = true;
        };

        capy::run_async(ex)(waiter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec == std::errc::bad_file_descriptor);
    }

    void testWaitOnUnopenedSocket()
    {
        checkClosedWait<tcp_socket>();
        checkClosedWait<udp_socket>();
        checkClosedWait<local_stream_socket>();
#if BOOST_COROSIO_POSIX
        // No local datagram sockets on Windows.
        checkClosedWait<local_datagram_socket>();
#endif
    }

    template<class Socket>
    void checkClosedReadWrite()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();

        Socket sock(ioc);

        std::error_code read_ec;
        std::error_code write_ec;
        bool done = false;

        auto io = [&]() -> capy::task<> {
            char buf[4] = {};
            auto [rec, rn] =
                co_await sock.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            read_ec = rec;
            BOOST_TEST_EQ(rn, std::size_t(0));
            auto [wec, wn] =
                co_await sock.write_some(capy::const_buffer(buf, sizeof(buf)));
            write_ec = wec;
            BOOST_TEST_EQ(wn, std::size_t(0));
            done = true;
        };

        capy::run_async(ex)(io());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(read_ec == std::errc::bad_file_descriptor);
        BOOST_TEST(write_ec == std::errc::bad_file_descriptor);
    }

    void testReadWriteOnUnopenedSocket()
    {
        checkClosedReadWrite<tcp_socket>();
        checkClosedReadWrite<local_stream_socket>();
    }

    void run()
    {
        testWaitOnUnopenedSocket();
        testReadWriteOnUnopenedSocket();
    }
};

COROSIO_BACKEND_TESTS(wait_closed_test, "boost.corosio.wait_closed")

// A faulted socket's wait(error) must name why it faulted: the delivered
// error_code must be a real, non-empty code (SO_ERROR, e.g.
// connection_reset), never an empty error_code — which is
// indistinguishable from a benign readiness signal — and never the
// canceled condition.
//
// Scoped to epoll (the control, which reads SO_ERROR and names the code)
// and io_uring (which completes the POLL_ADD with res>=0 and therefore an
// empty error_code — the bug). Not run on select, where a peer RST does
// not set except_fds and the error wait would never fire, nor on kqueue,
// which is not exercised on this host.
#if BOOST_COROSIO_HAS_EPOLL && BOOST_COROSIO_HAS_IO_URING
struct error_wait_names_reset_test
{
    template<auto Backend>
    void check()
    {
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        // Both ends linger with a zero timeout, so closing the peer
        // sends an RST rather than a graceful FIN.
        auto [s1, s2] = test::make_socket_pair(ioc);

        std::error_code wait_ec;
        bool wait_done = false;

        auto waiter = [&]() -> capy::task<> {
            auto [ec] = co_await s1.wait(wait_type::error);
            wait_ec   = ec;
            wait_done = true;
        };
        // Spawn order is park order: the error wait is outstanding
        // before the peer's RST reaches the socket.
        auto resetter = [&]() -> capy::task<> {
            s2.close();
            co_return;
        };

        capy::run_async(ex)(waiter());
        capy::run_async(ex)(resetter());
        ioc.run();

        BOOST_TEST(wait_done);
        BOOST_TEST(wait_ec);
        BOOST_TEST(wait_ec != capy::cond::canceled);
    }

    void run()
    {
        check<epoll>();     // control: names the code, passes
        check<io_uring>();  // bug D1: empty error_code, fails
    }
};

TEST_SUITE(error_wait_names_reset_test, "boost.corosio.wait_error_reset");
#endif

} // namespace boost::corosio
