//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Destroying the io_context while reactor operations are still parked
// must drain them without resuming or touching the awaiting coroutines.
// teardown_inflight.cpp covers the same contract on io_uring.

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/posix_stream_descriptor.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/wait_type.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <chrono>
#include <tuple>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

namespace {

// pipe2() is not on Darwin.
[[maybe_unused]] bool
make_nonblocking_pipe(int (&fds)[2])
{
    if (::pipe(fds) != 0)
        return false;
    for (int fd : fds)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}

// One pass that stops at EAGAIN is not enough: the peer's delayed ACK
// (about 40 ms on Linux loopback, longer on Darwin) later frees the
// whole send buffer, and a parked write would complete. The test only
// passed natively because it finished before that ACK; under valgrind
// it does not. Refill until a pass after a wait longer than any
// delayed-ACK timer (200 ms maximum on Linux) sends nothing; the small
// buffers bound how much that takes.
[[maybe_unused]] void
fill_fd(int fd, int peer)
{
    int size = 8192;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    ::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    char junk[4096] = {};
    for (bool sent = true; sent;)
    {
        sent = false;
        while (::send(fd, junk, sizeof(junk), MSG_DONTWAIT | MSG_NOSIGNAL) > 0)
            sent = true;
        pollfd pfd{fd, POLLOUT, 0};
        std::ignore = ::poll(&pfd, 1, 250);
    }
}

[[maybe_unused]] void
fill_pipe(int fd)
{
    char junk[4096] = {};
    while (::write(fd, junk, sizeof(junk)) > 0)
    {
    }
}

} // namespace

template<auto Backend>
struct reactor_teardown_test
{
    // Starts every coroutine and lets each op park; nothing it waits
    // for ever happens, so the bound is what returns.
    static void park(io_context& ioc)
    {
        std::ignore = ioc.run_for(std::chrono::milliseconds(20));
    }

    void testDestroyWithParkedDescriptorOps()
    {
        // One op per slot: transfer each way, and a wait per direction
        // plus the error wait. The raw counterpart ends stay open past
        // the context, so nothing parked ever becomes ready.
        int rp[2], wp[2], wr[2], ww[2], we[2];
        BOOST_TEST(make_nonblocking_pipe(rp));
        BOOST_TEST(make_nonblocking_pipe(wp));
        BOOST_TEST(make_nonblocking_pipe(wr));
        BOOST_TEST(make_nonblocking_pipe(ww));
        BOOST_TEST(make_nonblocking_pipe(we));
        fill_pipe(wp[1]);
        fill_pipe(ww[1]);

        int resumed = 0;
        {
            io_context ioc(Backend);
            auto reader = [&]() -> capy::task<> {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(rp[0]));
                char buf[16];
                std::ignore = co_await d.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                ++resumed;
            };
            auto writer = [&]() -> capy::task<> {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(wp[1]));
                char big[4096] = {};
                std::ignore =
                    co_await d.write_some(capy::const_buffer(big, sizeof(big)));
                ++resumed;
            };
            auto waiter = [&](int fd, wait_type w) -> capy::task<> {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(fd));
                std::ignore = co_await d.wait(w);
                ++resumed;
            };
            auto ex = ioc.get_executor();
            capy::run_async(ex)(reader());
            capy::run_async(ex)(writer());
            capy::run_async(ex)(waiter(wr[0], wait_type::read));
            capy::run_async(ex)(waiter(ww[1], wait_type::write));
            capy::run_async(ex)(waiter(we[0], wait_type::error));
            park(ioc);
        }
        BOOST_TEST_EQ(resumed, 0);
        for (int fd : {rp[1], wp[0], wr[1], ww[0], we[1]})
            ::close(fd);
    }

    void testDestroyWithParkedSocketOps()
    {
        int resumed = 0;
        {
            io_context ioc(Backend);
            // Made outside the coroutines (it drains the context); each
            // socket moves into a frame so its fd stays open when the
            // frame is abandoned.
            auto [w1, w2] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
            auto [r1, r2] =
                test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc);
            fill_fd(
                static_cast<int>(w1.native_handle()),
                static_cast<int>(w2.native_handle()));
            auto writer = [](tcp_socket s, int& count) -> capy::task<> {
                char big[65536] = {};
                std::ignore =
                    co_await s.write_some(capy::const_buffer(big, sizeof(big)));
                ++count;
            }(std::move(w1), resumed);
            // A separate, idle pair: the filled one has data to read.
            auto reader = [](tcp_socket s, int& count) -> capy::task<> {
                char buf[16];
                std::ignore = co_await s.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                ++count;
            }(std::move(r1), resumed);
            auto ex = ioc.get_executor();
            capy::run_async(ex)(std::move(writer));
            capy::run_async(ex)(std::move(reader));
            park(ioc);
        }
        BOOST_TEST_EQ(resumed, 0);
    }

    void testDestroyDrainsBothEndsOfOnePipe()
    {
        // Shutdown closes one end while the other end's op is still
        // parked. A write parked on the full end must not be re-run
        // against the closed reader: that would raise SIGPIPE.
        int p[2];
        BOOST_TEST(make_nonblocking_pipe(p));
        fill_pipe(p[1]);

        int resumed = 0;
        {
            io_context ioc(Backend);
            auto reader = [&]() -> capy::task<> {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(p[0]));
                // Parks on a wait, since the pipe is full and a read
                // would complete.
                std::ignore = co_await d.wait(wait_type::error);
                ++resumed;
            };
            auto writer = [&]() -> capy::task<> {
                posix_stream_descriptor d(ioc);
                BOOST_TEST(!d.assign(p[1]));
                char big[4096] = {};
                std::ignore =
                    co_await d.write_some(capy::const_buffer(big, sizeof(big)));
                ++resumed;
            };
            auto ex = ioc.get_executor();
            capy::run_async(ex)(reader());
            capy::run_async(ex)(writer());
            park(ioc);
        }
        BOOST_TEST_EQ(resumed, 0);
    }

    void run()
    {
#if !COROSIO_TEST_HAS_ASAN
        testDestroyWithParkedDescriptorOps();
        testDestroyWithParkedSocketOps();
        testDestroyDrainsBothEndsOfOnePipe();
#endif
    }
};

COROSIO_REACTOR_BACKEND_TESTS(
    reactor_teardown_test, "boost.corosio.reactor_teardown")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
