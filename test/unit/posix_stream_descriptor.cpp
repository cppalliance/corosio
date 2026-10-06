//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/posix_stream_descriptor.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/delay.hpp>
#include <boost/corosio/error.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <typeinfo>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if BOOST_COROSIO_HAS_KQUEUE
#include <sys/event.h>
#endif

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

static_assert(capy::ReadStream<posix_stream_descriptor>);
static_assert(capy::WriteStream<posix_stream_descriptor>);
static_assert(std::is_base_of_v<io_stream, posix_stream_descriptor>);
static_assert(!std::is_copy_constructible_v<posix_stream_descriptor>);
static_assert(std::is_move_constructible_v<posix_stream_descriptor>);

// assign() must be impossible to call and ignore.
static_assert(std::is_same_v<
              decltype(std::declval<posix_stream_descriptor&>().assign(
                  std::declval<native_handle_type>())),
              std::error_code>);

struct posix_stream_descriptor_contract_test
{
    void run() {}
};

TEST_SUITE(
    posix_stream_descriptor_contract_test,
    "boost.corosio.posix_stream_descriptor_contract");

template<auto Backend>
struct posix_stream_descriptor_test
{
    // Returns a pipe whose ends are both blocking, so every test that
    // cares about O_NONBLOCK starts from a known state.
    static void make_pipe(int (&fds)[2])
    {
        BOOST_TEST_EQ(::pipe(fds), 0);
    }

    // io_uring never touches the descriptor's flags; the reactors set
    // O_NONBLOCK on the first transfer.
#if BOOST_COROSIO_HAS_URING
    static constexpr bool is_uring =
        std::is_same_v<std::remove_const_t<decltype(Backend)>, uring_t>;
#else
    static constexpr bool is_uring = false;
#endif

    static bool has_nonblock(int fd)
    {
        return (::fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
    }

    // The .epoll, .select and .uring variants are separate ctest
    // entries that run concurrently under ctest -j, so a fixed global
    // name would race between them.
    static std::filesystem::path temp_path(char const* tag)
    {
        return std::filesystem::temp_directory_path() /
            ("corosio_posix_stream_descriptor_" + std::string(tag) + "_" +
             typeid(decltype(Backend)).name() + "_" +
             std::to_string(::getpid()));
    }

    void testConstruction()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        BOOST_TEST_EQ(d.is_open(), false);
        BOOST_TEST_EQ(d.native_handle(), -1);
    }

    void testAssignRejectsRegularFile()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);

        auto path = temp_path("regular");
        int fd    = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        BOOST_TEST(fd >= 0);

        BOOST_TEST(d.assign(fd) == std::errc::operation_not_supported);
        // Rejection leaves the object closed and the fd with the caller.
        BOOST_TEST_EQ(d.is_open(), false);
        BOOST_TEST_EQ(::fcntl(fd, F_GETFD) >= 0, true);

        ::close(fd);
        std::filesystem::remove(path);
    }

    void testAssignRejectsBadFd()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        BOOST_TEST(d.assign(-1) == std::errc::bad_file_descriptor);
    }

    void testAssignRejectsSelf()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);

        BOOST_TEST(!d.assign(fds[0]));
        BOOST_TEST(d.assign(d.native_handle()) == error::already_open);
        // Still open on the same fd after the rejected self-assign.
        BOOST_TEST_EQ(d.is_open(), true);
        BOOST_TEST_EQ(d.native_handle(), fds[0]);

        ::close(fds[1]);
    }

    void testAssignOnOpenKeepsParkedRead()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int a[2], b[2];
        make_pipe(a);
        make_pipe(b);
        BOOST_TEST(!d.assign(a[0]));

        std::error_code rec;
        std::size_t rn = 0;
        char buf[1];
        auto reader = [&]() -> capy::task<> {
            auto [e, n] = co_await d.read_some(capy::mutable_buffer(buf, 1));
            rec         = e;
            rn          = n;
        };
        auto other = [&]() -> capy::task<> {
            auto [e] = co_await delay(std::chrono::milliseconds(10));
            (void)e;
            BOOST_TEST(d.assign(b[0]) == error::already_open);
            BOOST_TEST_EQ(d.native_handle(), a[0]);
            BOOST_TEST(::fcntl(b[0], F_GETFD) >= 0); // still the caller's
            BOOST_TEST_EQ(::write(a[1], "k", 1), 1);
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(other());
        ioc.run();

        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 1u);
        ::close(a[1]);
        ::close(b[0]);
        ::close(b[1]);
    }

    void testAssignDoesNotTouchFlagsButReadDoes()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);

        BOOST_TEST(!d.assign(fds[0]));
        // The single most important contract: adoption is non-mutating.
        BOOST_TEST_EQ(::fcntl(fds[0], F_GETFL) & O_NONBLOCK, 0);

        char const msg[] = "hello";
        BOOST_TEST_EQ(::write(fds[1], msg, 5), 5);

        char buf[16]{};
        std::size_t n = 0;
        auto reader   = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!rec);
            n = rn;
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();

        BOOST_TEST_EQ(n, 5u);
        BOOST_TEST_EQ(std::memcmp(buf, msg, 5), 0);
        // ...and on a reactor the first read is what flipped the flag.
        BOOST_TEST_EQ(has_nonblock(fds[0]), !is_uring);

        ::close(fds[1]);
    }

    void testWaitDoesNotTouchFlags()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        char const msg[] = "x";
        BOOST_TEST_EQ(::write(fds[1], msg, 1), 1);

        std::error_code ec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::read);
            ec         = wec;
            done       = true;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!ec);
        // wait() alone must never modify a descriptor someone else owns.
        BOOST_TEST_EQ(::fcntl(fds[0], F_GETFL) & O_NONBLOCK, 0);
        // ...and it consumed nothing.
        char buf[4]{};
        BOOST_TEST_EQ(::read(fds[0], buf, 1), 1);

        ::close(fds[1]);
    }

    void testWaitReadParksUntilDataArrives()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        bool ready = false;
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::read);
            ec         = wec;
            ready      = true;
        };
        auto writer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            char const msg[] = "z";
            BOOST_TEST_EQ(::write(fds[1], msg, 1), 1);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(writer());
        ioc.run();

        BOOST_TEST(ready);
        BOOST_TEST(!ec);
        ::close(fds[1]);
    }

    void testWaitWriteOnFullPipe()
    {
        // Registration latches write_ready on an empty pipe; the probe
        // must not let that stale flag report a full pipe as writable.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));

        // Run the reactor once while the pipe is still empty, so the
        // registration's writable edge is consumed with nothing parked
        // and write_ready is latched.
        auto settle = [&]() -> capy::task<> {
            auto [e] = co_await delay(std::chrono::milliseconds(1));
            (void)e;
        };
        capy::run_async(ioc.get_executor())(settle());
        ioc.run();
        ioc.restart();

        // Fill the pipe. The write end must be nonblocking to do this
        // without deadlocking, and this is the caller's own fd, so the
        // test sets the flag itself rather than relying on the library.
        int flags = ::fcntl(fds[1], F_GETFL);
        BOOST_TEST_EQ(::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK), 0);
        char block[4096];
        std::memset(block, 'a', sizeof(block));
        while (::write(fds[1], block, sizeof(block)) > 0)
        {
        }

        bool ready  = false;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::write);
            BOOST_TEST(!wec);
            ready = true;
        };
        auto drainer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            BOOST_TEST_EQ(ready, false); // still parked on the full pipe
            char sink[8192];
            BOOST_TEST(::read(fds[0], sink, sizeof(sink)) > 0);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(drainer());
        ioc.run();

        BOOST_TEST(ready);
        ::close(fds[0]);
    }

    void testWriteSome()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));

        std::size_t n = 0;
        auto writer   = [&]() -> capy::task<> {
            auto [wec, wn] =
                co_await d.write_some(capy::const_buffer("abc", 3));
            BOOST_TEST(!wec);
            n = wn;
        };
        capy::run_async(ioc.get_executor())(writer());
        ioc.run();

        BOOST_TEST_EQ(n, 3u);
        char buf[4]{};
        BOOST_TEST_EQ(::read(fds[0], buf, 3), 3);
        BOOST_TEST_EQ(std::memcmp(buf, "abc", 3), 0);

        ::close(fds[0]);
    }

    void testCancelPendingRead()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        std::error_code ec;
        bool done = false;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec   = rec;
            done = true;
            (void)rn;
        };
        auto canceller = [&]() -> capy::task<> {
            d.cancel();
            co_return;
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(reader());
        capy::run_async(ex)(canceller());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == capy::cond::canceled);
        ::close(fds[1]);
    }

    void testReleaseCancelsAndTransfersOwnership()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        std::error_code ec;
        bool done = false;
        char buf[8]{};
        native_handle_type released = -1;
        auto reader                 = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec   = rec;
            done = true;
            (void)rn;
        };
        auto releaser = [&]() -> capy::task<> {
            released = d.release();
            co_return;
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(reader());
        capy::run_async(ex)(releaser());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == capy::cond::canceled);
        BOOST_TEST_EQ(d.is_open(), false);
        BOOST_TEST_EQ(released, fds[0]);
        // The released fd is still open and ours to close.
        BOOST_TEST(::fcntl(released, F_GETFD) >= 0);
        ::close(released);
        ::close(fds[1]);
    }

    void testEofOnClosedWriteEnd()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));
        ::close(fds[1]);

        std::error_code ec;
        std::size_t n = 1;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = rec;
            n  = rn;
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();

        BOOST_TEST_EQ(n, 0u);
        BOOST_TEST(ec == capy::cond::eof);
    }

    void testParkedReadSeesEofWhenWriterCloses()
    {
        // Distinct from testEofOnClosedWriteEnd, where the write end is
        // already closed when read_some() runs and ::read returns 0 on
        // the fast path without any backend event. Here the read
        // genuinely parks first, so the hangup has to arrive as a
        // backend event: epoll reports EPOLLHUP alone for a pipe read
        // end -- no EPOLLIN, no EPOLLERR -- and the registration is
        // edge-triggered, so a mapping that drops it hangs forever.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        bool done = false;
        std::error_code ec;
        std::size_t n = 1;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec   = rec;
            n    = rn;
            done = true;
        };
        auto closer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            BOOST_TEST_EQ(done, false); // still parked on the empty pipe
            ::close(fds[1]);

            // Bound the park so a backend that never delivers the
            // hangup fails the assertions below instead of wedging
            // ioc.run() for good.
            for (int i = 0; i < 50 && !done; ++i)
                co_await delay(std::chrono::milliseconds(10));
            if (!done)
                d.cancel();
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(reader());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == capy::cond::eof);
        BOOST_TEST_EQ(n, 0u);
    }

    void testWaitReadOnHungUpPipeIsReadiness()
    {
        // A pipe whose writer closed is readable-at-EOF, not faulted:
        // the wait-then-read idiom the guide teaches must hand the
        // caller a clean wait and let the following read report eof.
        // The reactors reach this through poll(POLLIN) returning
        // POLLHUP; io_uring through a POLL_ADD revents carrying
        // POLLHUP, which must not be turned into a fault.
        for (bool close_before_wait : {true, false})
        {
            io_context ioc(Backend);
            posix_stream_descriptor d(ioc);
            int fds[2];
            make_pipe(fds);
            BOOST_TEST(!d.assign(fds[0]));
            if (close_before_wait)
                ::close(fds[1]);

            bool done = false;
            std::error_code ec;
            auto waiter = [&]() -> capy::task<> {
                auto [wec] = co_await d.wait(wait_type::read);
                ec         = wec;
                done       = true;
            };
            auto closer = [&]() -> capy::task<> {
                co_await delay(std::chrono::milliseconds(10));
                if (!close_before_wait)
                {
                    BOOST_TEST_EQ(done, false); // parked, pipe still open
                    ::close(fds[1]);
                }
                for (int i = 0; i < 50 && !done; ++i)
                    co_await delay(std::chrono::milliseconds(10));
                if (!done)
                    d.cancel();
            };

            auto ex = ioc.get_executor();
            capy::run_async(ex)(waiter());
            capy::run_async(ex)(closer());
            ioc.run();

            BOOST_TEST(done);
            BOOST_TEST(!ec);
        }
    }

    void testGatherReadWrite()
    {
        // Every other case here is single-buffer, which takes the
        // ::read / write_one fast path; this is the only coverage of
        // the ::readv / ::writev gather forms.
        io_context ioc(Backend);
        posix_stream_descriptor w(ioc);
        posix_stream_descriptor r(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!w.assign(fds[1]));
        BOOST_TEST(!r.assign(fds[0]));

        char const head[] = "hello ";
        char const tail[] = "world";
        char got_head[6]{};
        char got_tail[5]{};
        std::size_t wn = 0;
        std::size_t rn = 0;

        auto body = [&]() -> capy::task<> {
            std::array<capy::const_buffer, 2> out = {
                capy::const_buffer(head, 6), capy::const_buffer(tail, 5)};
            auto [wec, n1] = co_await w.write_some(out);
            BOOST_TEST(!wec);
            wn = n1;

            std::array<capy::mutable_buffer, 2> in = {
                capy::mutable_buffer(got_head, sizeof(got_head)),
                capy::mutable_buffer(got_tail, sizeof(got_tail))};
            auto [rec, n2] = co_await r.read_some(in);
            BOOST_TEST(!rec);
            rn = n2;
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();

        BOOST_TEST_EQ(wn, 11u);
        BOOST_TEST_EQ(rn, 11u);
        BOOST_TEST_EQ(std::memcmp(got_head, "hello ", 6), 0);
        BOOST_TEST_EQ(std::memcmp(got_tail, "world", 5), 0);
    }

    void testReassignRearmsNonblockingLatch()
    {
        // A stale nonblocking_ == true would leave the newly adopted
        // descriptor blocking while the library believed otherwise.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int first[2];
        int second[2];
        make_pipe(first);
        make_pipe(second);
        BOOST_TEST(!d.assign(first[0]));

        BOOST_TEST_EQ(::write(first[1], "a", 1), 1);

        char buf[8]{};
        auto body = [&]() -> capy::task<> {
            auto [ec1, n1] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec1);
            BOOST_TEST_EQ(n1, 1u);
            BOOST_TEST_EQ(has_nonblock(first[0]), !is_uring);

            // Re-adopting after close() must not inherit the latch; the
            // fresh pipe end is still blocking.
            d.close();
            BOOST_TEST(!d.assign(second[0]));
            BOOST_TEST_EQ(::fcntl(second[0], F_GETFL) & O_NONBLOCK, 0);

            BOOST_TEST_EQ(::write(second[1], "xy", 2), 2);
            auto [ec2, n2] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 2u);
            BOOST_TEST_EQ(std::memcmp(buf, "xy", 2), 0);
            BOOST_TEST_EQ(has_nonblock(second[0]), !is_uring);
        };
        capy::run_async(ioc.get_executor())(body());
        ioc.run();

        ::close(first[1]);
        ::close(second[1]);
    }

    void testParkedWriteOnBrokenPipeReportsEpipe()
    {
        // The reactor's error probe is getsockopt(SO_ERROR), which fails
        // with ENOTSOCK on a pipe. The write must genuinely park before
        // the peer closes: a fast-path completion (e.g. closing the read
        // end before writing) never reaches invoke_deferred_io's
        // ENOTSOCK arm and would pass even with the bug present.
        //
        // Every suite shares one process, so the disposition has to go
        // back the way it was found.
        struct sigpipe_guard
        {
            void (*prev)(int) = ::signal(SIGPIPE, SIG_IGN);
            ~sigpipe_guard()
            {
                ::signal(SIGPIPE, prev);
            }
        } restore_sigpipe;

        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));

        // Fill the pipe so write_some() genuinely parks. The write end
        // is the caller's own fd, so the test sets O_NONBLOCK itself
        // rather than relying on the library (see testWaitWriteOnFullPipe).
        int flags = ::fcntl(fds[1], F_GETFL);
        BOOST_TEST_EQ(::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK), 0);
        char block[4096];
        std::memset(block, 'a', sizeof(block));
        while (::write(fds[1], block, sizeof(block)) > 0)
        {
        }

        bool done = false;
        std::error_code ec;
        auto writer = [&]() -> capy::task<> {
            auto [wec, wn] = co_await d.write_some(capy::const_buffer("x", 1));
            ec             = wec;
            done           = true;
            (void)wn;
        };
        auto closer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            BOOST_TEST_EQ(done, false); // still parked on the full pipe
            ::close(fds[0]);
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(writer());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == std::errc::broken_pipe);
        BOOST_TEST(ec != std::errc::not_a_socket);
    }

    void testWaitErrorParksThenNamesRealCode()
    {
        // Same ENOTSOCK hazard as the write case above, but through the
        // wait_error_op arm: wait(error) must genuinely park (the fd is
        // still healthy when wait() probes it) so the later dispatch
        // reaches invoke_deferred_io rather than the speculative
        // fast-path probe in do_wait(), which never touches SO_ERROR.
        //
        // Two backends never wake this wait, so on them the bound
        // below is what ends it and the error assertions are skipped.
        // select's exceptional set does not cover a pipe whose peer
        // closed (verified: except_fds never comes back set for it,
        // unlike a TCP reset). kqueue raises EV_EOF on both filters for
        // the hangup, with fflags == 0 on each (measured on Darwin), and
        // kqueue_scheduler raises reactor_event_error only when
        // fflags != 0; a TCP reset does set fflags, which is why
        // tcp_socket.cpp's
        // testWaitForErrorThenWait -- the precedent this copies --
        // needs no kqueue exemption.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));

        bool done = false;
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::error);
            ec         = wec;
            done       = true;
        };
        auto closer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            BOOST_TEST_EQ(done, false); // still parked, no error yet
            ::close(fds[0]);

            // Bound the wait: a backend that never reports this as an
            // exceptional condition would otherwise park it for good.
            for (int i = 0; i < 20 && !done; ++i)
                co_await delay(std::chrono::milliseconds(10));
            if (!done)
                d.cancel();
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(done);
#if BOOST_COROSIO_HAS_SELECT
        constexpr bool is_select =
            std::is_same_v<std::remove_const_t<decltype(Backend)>, select_t>;
#else
        constexpr bool is_select = false;
#endif
#if BOOST_COROSIO_HAS_KQUEUE
        constexpr bool is_kqueue =
            std::is_same_v<std::remove_const_t<decltype(Backend)>, kqueue_t>;
#else
        constexpr bool is_kqueue = false;
#endif
        if constexpr (!is_select && !is_kqueue)
        {
            // Never the cancel: that would say the close reached
            // nothing and the bound is what ended the wait.
            BOOST_TEST(ec != capy::cond::canceled);
            BOOST_TEST(ec == std::errc::io_error);
        }
    }

    void testWaitOnClosedDescriptor()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);

        std::error_code ec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::read);
            ec         = wec;
            done       = true;
        };
        capy::run_async(ioc.get_executor())(waiter());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == std::errc::bad_file_descriptor);
    }

    void testConcurrentFirstReadAndWrite()
    {
        // One read and one write may be in flight at once; on a reactor
        // their first calls both arm O_NONBLOCK, from different threads.
        int sv[2];
        BOOST_TEST_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

        io_context ioc(Backend, 2);
        posix_stream_descriptor d(ioc);
        BOOST_TEST(!d.assign(sv[0]));

        std::error_code rec, wec;
        char rbuf[1];
        auto reader = [&]() -> capy::task<> {
            auto [e, n] = co_await d.read_some(capy::mutable_buffer(rbuf, 1));
            rec         = e;
            (void)n;
        };
        auto writer = [&]() -> capy::task<> {
            auto [e, n] = co_await d.write_some(capy::const_buffer("x", 1));
            wec         = e;
            (void)n;
            BOOST_TEST_EQ(::write(sv[1], "y", 1), 1);
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(writer());
        std::thread t([&] { ioc.run(); });
        ioc.run();
        t.join();

        BOOST_TEST(!rec);
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(has_nonblock(sv[0]), !is_uring);
        ::close(sv[1]);
    }

    void testUnpollableCharDeviceAdoptsEverywhere()
    {
        // epoll (EPERM) and kqueue (EINVAL) cannot watch these, but
        // their I/O never blocks; every backend adopts them.
        io_context ioc(Backend);
        posix_stream_descriptor zero(ioc);
        posix_stream_descriptor null(ioc);
        int zfd = ::open("/dev/zero", O_RDONLY);
        int nfd = ::open("/dev/null", O_WRONLY);
        BOOST_TEST(zfd >= 0);
        BOOST_TEST(nfd >= 0);
        BOOST_TEST(!zero.assign(zfd));
        BOOST_TEST(!null.assign(nfd));

        std::error_code rec, wec, waitec;
        std::size_t rn = 0, wn = 0;
        char buf[16];
        std::memset(buf, 'x', sizeof(buf));
        auto t = [&]() -> capy::task<> {
            auto [e1, n1] =
                co_await zero.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = e1;
            rn  = n1;
            auto [e2, n2] =
                co_await null.write_some(capy::const_buffer(buf, sizeof(buf)));
            wec       = e2;
            wn        = n2;
            auto [e3] = co_await zero.wait(wait_type::read);
            waitec    = e3;
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();

        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, sizeof(buf));
        BOOST_TEST_EQ(buf[0], '\0');
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, sizeof(buf));
        BOOST_TEST(!waitec);
    }

    void testUnpollableWaitThatWouldParkIsNotSupported()
    {
        // epoll adopts /dev/zero unwatched and io_uring cannot poll it,
        // so a wait the readiness probe cannot satisfy would park
        // forever or fail with the kernel's EINVAL. kqueue and select
        // watch /dev/zero, where this wait does park.
        constexpr bool unwatched =
#if BOOST_COROSIO_HAS_EPOLL
            std::is_same_v<std::remove_const_t<decltype(Backend)>, epoll_t> ||
#endif
#if BOOST_COROSIO_HAS_URING
            std::is_same_v<std::remove_const_t<decltype(Backend)>, uring_t> ||
#endif
            false;
        if constexpr (unwatched)
        {
            io_context ioc(Backend);
            posix_stream_descriptor zero(ioc);
            BOOST_TEST(!zero.assign(::open("/dev/zero", O_RDONLY)));

            std::error_code ec;
            auto t = [&]() -> capy::task<> {
                auto [e] = co_await zero.wait(wait_type::error);
                ec       = e;
            };
            capy::run_async(ioc.get_executor())(t());
            ioc.run();

            BOOST_TEST(ec == std::errc::operation_not_supported);
        }
    }

    void testSelectFdSetsizeIsRejected()
    {
#if BOOST_COROSIO_HAS_SELECT
        if constexpr (
            std::is_same_v<std::remove_const_t<decltype(Backend)>, select_t>)
        {
            io_context ioc(Backend);
            posix_stream_descriptor d(ioc);
            int fds[2];
            make_pipe(fds);

            int high = ::fcntl(fds[1], F_DUPFD, FD_SETSIZE);
            if (high < 0)
            {
                ::close(fds[0]); // RLIMIT_NOFILE too low to test
                ::close(fds[1]);
                return;
            }
            BOOST_TEST(d.assign(high) == std::errc::too_many_files_open);
            BOOST_TEST_EQ(d.is_open(), false);
            BOOST_TEST(::fcntl(high, F_GETFD) != -1); // caller still owns it
            ::close(high);
            ::close(fds[0]);
            ::close(fds[1]);
        }
#endif
    }

    void testWaitErrorOnHungUpReadEnd()
    {
        // Hangup first, then wait: every backend completes with a code.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));
        ::close(fds[1]);

        std::error_code ec;
        bool done = false;
        auto t    = [&]() -> capy::task<> {
            auto [e] = co_await d.wait(wait_type::error);
            ec       = e;
            done     = true;
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(ec == std::errc::io_error);
    }

    void testWaitErrorParksOnReadEndThenNamesCode()
    {
        waitErrorParksOnReadEnd(false);
        // Data still unread when the writer closes: epoll reports
        // EPOLLIN|EPOLLHUP, which must reach the error wait too.
        waitErrorParksOnReadEnd(true);
    }

    void waitErrorParksOnReadEnd(bool leave_data)
    {
        // The read-end twin of testWaitErrorParksThenNamesRealCode:
        // epoll reports a writer close on the read end as EPOLLHUP
        // alone, which must still reach the parked error wait. select
        // and kqueue raise no event for it, as described there.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        bool done = false;
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [wec] = co_await d.wait(wait_type::error);
            ec         = wec;
            done       = true;
        };
        auto closer = [&]() -> capy::task<> {
            co_await delay(std::chrono::milliseconds(10));
            BOOST_TEST_EQ(done, false); // still parked, no error yet
            if (leave_data)
                BOOST_TEST_EQ(::write(fds[1], "x", 1), 1);
            ::close(fds[1]);

            for (int i = 0; i < 20 && !done; ++i)
                co_await delay(std::chrono::milliseconds(10));
            if (!done)
                d.cancel();
        };

        auto ex = ioc.get_executor();
        capy::run_async(ex)(waiter());
        capy::run_async(ex)(closer());
        ioc.run();

        BOOST_TEST(done);
#if BOOST_COROSIO_HAS_SELECT
        constexpr bool is_select =
            std::is_same_v<std::remove_const_t<decltype(Backend)>, select_t>;
#else
        constexpr bool is_select = false;
#endif
#if BOOST_COROSIO_HAS_KQUEUE
        constexpr bool is_kqueue =
            std::is_same_v<std::remove_const_t<decltype(Backend)>, kqueue_t>;
#else
        constexpr bool is_kqueue = false;
#endif
        if constexpr (!is_select && !is_kqueue)
        {
            BOOST_TEST(ec != capy::cond::canceled);
            BOOST_TEST(ec == std::errc::io_error);
        }
    }

    void testSelectDoesNotSpinOnReadableIdleFd()
    {
        // An adopted, always-readable fd with nothing parked on it must
        // not keep the reactor busy while a timer is outstanding.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fd = ::open("/dev/zero", O_RDONLY);
        BOOST_TEST(!d.assign(fd));

        std::clock_t const c0 = std::clock();
        auto t                = [&]() -> capy::task<> {
            auto [e] = co_await delay(std::chrono::milliseconds(200));
            (void)e;
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        double const cpu_ms =
            1000.0 * double(std::clock() - c0) / CLOCKS_PER_SEC;
        BOOST_TEST(cpu_ms < 100.0);
    }

    void testWriteSomeArmsNonblocking()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));
        BOOST_TEST(!(::fcntl(fds[1], F_GETFL) & O_NONBLOCK));

        std::error_code ec;
        auto writer = [&]() -> capy::task<> {
            auto [wec, wn] = co_await d.write_some(capy::const_buffer("y", 1));
            ec             = wec;
            (void)wn;
        };
        capy::run_async(ioc.get_executor())(writer());
        ioc.run();

        BOOST_TEST(!ec);
        BOOST_TEST_EQ(has_nonblock(fds[1]), !is_uring);
        ::close(fds[0]);
    }

    void testStopTokenCancelsRead()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        std::stop_source stop;
        std::error_code ec;
        char buf[4];
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            (void)n;
        };
        auto stopper = [&]() -> capy::task<> {
            auto [e] = co_await delay(std::chrono::milliseconds(10));
            (void)e;
            stop.request_stop();
        };
        capy::run_async(ioc.get_executor(), stop.get_token())(reader());
        capy::run_async(ioc.get_executor())(stopper());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
        ::close(fds[1]);
    }

    void testZeroLength()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        std::error_code ec;
        std::size_t n = 1;
        auto t        = [&]() -> capy::task<> {
            auto [e, rn] = co_await d.read_some(capy::mutable_buffer());
            ec           = e;
            n            = rn;
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 0u);
        ::close(fds[1]);
    }

    void testReleaseWhenClosedThrows()
    {
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        BOOST_TEST_THROWS(d.release(), std::system_error);
    }

    void testReleaseThenReadoptElsewhere()
    {
        int fds[2];
        make_pipe(fds);
        io_context a(Backend);
        io_context b(Backend);
        posix_stream_descriptor da(a);
        posix_stream_descriptor db(b);
        BOOST_TEST(!da.assign(fds[0]));
        int const fd = da.release();
        BOOST_TEST(!db.assign(fd));

        BOOST_TEST_EQ(::write(fds[1], "k", 1), 1);
        char c = 0;
        std::error_code ec;
        auto t = [&]() -> capy::task<> {
            auto [e, n] = co_await db.read_some(capy::mutable_buffer(&c, 1));
            ec          = e;
            (void)n;
        };
        capy::run_async(b.get_executor())(t());
        b.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(c, 'k');
        ::close(fds[1]);
    }

    void testComposedRead()
    {
        // The design's central claim: capy::read works on a descriptor.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        std::string got(10, '\0');
        std::error_code ec;
        auto reader = [&]() -> capy::task<> {
            auto [e, n] = co_await capy::read(
                d, capy::mutable_buffer(got.data(), got.size()));
            ec = e;
            (void)n;
        };
        auto writer = [&]() -> capy::task<> {
            for (char const* part : {"hello", "world"})
            {
                auto [e] = co_await delay(std::chrono::milliseconds(5));
                (void)e;
                BOOST_TEST_EQ(::write(fds[1], part, 5), 5);
            }
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(writer());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(got, "helloworld");
        ::close(fds[1]);
    }

    void testAdoptPipeWhosePeerIsGone()
    {
        // Draining an exited child's stdout: the writer is gone before
        // assign(). FreeBSD's kqueue refuses EVFILT_WRITE on such a
        // pipe with EPIPE, which must not refuse the adoption.
        io_context ioc(Backend);
        posix_stream_descriptor rd(ioc);
        posix_stream_descriptor wr(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST_EQ(::write(fds[1], "hi", 2), 2);
        ::close(fds[1]);
        BOOST_TEST(!rd.assign(fds[0]));

        int gone[2];
        make_pipe(gone);
        ::close(gone[0]);
        BOOST_TEST(!wr.assign(gone[1]));

        char buf[8]{};
        std::size_t n = 0;
        std::error_code ec, eof_ec;
        auto reader = [&]() -> capy::task<> {
            auto [e1, n1] =
                co_await rd.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e1;
            n  = n1;
            auto [e2, n2] =
                co_await rd.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            eof_ec = e2;
            (void)n2;
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();

        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);
        BOOST_TEST(eof_ec == capy::cond::eof);
    }

    void waitWriteWithReaderGone(bool parked)
    {
        // A readiness wait reports the hangup as readiness, as asio does;
        // the write that follows names broken_pipe.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));
        if (parked)
        {
            // Full, so the wait genuinely parks. The caller's own fd, so
            // the test sets O_NONBLOCK itself.
            int flags = ::fcntl(fds[1], F_GETFL);
            BOOST_TEST_EQ(::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK), 0);
            char block[4096] = {};
            while (::write(fds[1], block, sizeof(block)) > 0)
            {
            }
        }
        else
        {
            ::close(fds[0]);
        }

        std::error_code ec;
        bool done   = false;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await d.wait(wait_type::write);
            ec       = e;
            done     = true;
        };
        auto closer = [&]() -> capy::task<> {
            auto [e] = co_await delay(std::chrono::milliseconds(10));
            (void)e;
            if (parked)
            {
                BOOST_TEST_EQ(done, false);
                ::close(fds[0]);
            }
            for (int i = 0; i < 20 && !done; ++i)
            {
                auto [e2] = co_await delay(std::chrono::milliseconds(10));
                (void)e2;
            }
            if (!done)
                d.cancel();
        };
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(closer());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!ec);
    }

    void testWaitWriteWithReaderGoneIsReadiness()
    {
        waitWriteWithReaderGone(false);
        waitWriteWithReaderGone(true);
    }

    void testReaderlessPipeAdoptsAndWriteFails()
    {
        // A reader-less pipe write end refuses EVFILT_WRITE on FreeBSD;
        // with lazy write registration, adoption never asks for it.
        io_context ioc(Backend);
        posix_stream_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        ::close(fds[0]);
        BOOST_TEST(!d.assign(fds[1]));

        std::error_code ec;
        auto t = [&]() -> capy::task<> {
            auto [e, n] = co_await d.write_some(capy::const_buffer("x", 1));
            ec          = e;
            (void)n;
        };
        struct sigpipe_ignore
        {
            void (*prev)(int) = ::signal(SIGPIPE, SIG_IGN);
            ~sigpipe_ignore()
            {
                ::signal(SIGPIPE, prev);
            }
        } guard;
        capy::run_async(ioc.get_executor())(t());
        ioc.run();
        BOOST_TEST(ec == std::errc::broken_pipe);
    }

    void testKqueueFdAdoptsWithoutWriteFilter()
    {
#if BOOST_COROSIO_HAS_KQUEUE
        if constexpr (
            std::is_same_v<std::remove_const_t<decltype(Backend)>, kqueue_t>)
        {
            // A kqueue descriptor supports EVFILT_READ only, so it
            // stands in for a read-only device: adoption must not ask
            // for a write filter, and a write wait that would park
            // fails with the kernel's refusal. Darwin builds poll() on
            // kqueue and reports POLLNVAL for the refused filter, so
            // there the speculative probe ends the write wait before
            // it can park.
            io_context ioc(Backend);
            posix_stream_descriptor d(ioc);
            int inner = ::kqueue();
            BOOST_TEST(inner >= 0);
            BOOST_TEST(!d.assign(inner));

            struct kevent ev;
            EV_SET(&ev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            BOOST_TEST_EQ(::kevent(inner, &ev, 1, nullptr, 0, nullptr), 0);
            EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
            BOOST_TEST_EQ(::kevent(inner, &ev, 1, nullptr, 0, nullptr), 0);

            std::error_code rec;
            auto r = [&]() -> capy::task<> {
                auto [e] = co_await d.wait(wait_type::read);
                rec      = e;
            };
            capy::run_async(ioc.get_executor())(r());
            ioc.run();
            BOOST_TEST(!rec);

#if !defined(__APPLE__)
            std::error_code wec;
            auto w = [&]() -> capy::task<> {
                auto [e] = co_await d.wait(wait_type::write);
                wec      = e;
            };
            ioc.restart();
            capy::run_async(ioc.get_executor())(w());
            ioc.run();
            BOOST_TEST(wec == std::errc::invalid_argument);
#endif
        }
#endif
    }

    void testBlockingFdReadIsCancellable()
    {
        // The fd is left blocking on io_uring; an in-flight read must
        // still end on cancel(), a stop token, and close().
        for (int how = 0; how < 3; ++how)
        {
            io_context ioc(Backend);
            posix_stream_descriptor d(ioc);
            int fds[2];
            make_pipe(fds);
            BOOST_TEST(!d.assign(fds[0]));

            std::stop_source stop;
            std::error_code ec;
            char buf[1];
            auto reader = [&]() -> capy::task<> {
                auto [e, n] =
                    co_await d.read_some(capy::mutable_buffer(buf, 1));
                ec = e;
                (void)n;
            };
            auto ender = [&]() -> capy::task<> {
                auto [e] = co_await delay(std::chrono::milliseconds(10));
                (void)e;
                if (how == 0)
                    d.cancel();
                else if (how == 1)
                    stop.request_stop();
                else
                    d.close();
            };
            capy::run_async(ioc.get_executor(), stop.get_token())(reader());
            capy::run_async(ioc.get_executor())(ender());
            ioc.run();
            BOOST_TEST(ec == capy::cond::canceled);
            ::close(fds[1]);
        }
    }

    void run()
    {
        testConstruction();
        testAssignRejectsRegularFile();
        testAssignRejectsBadFd();
        testAssignRejectsSelf();
        testAssignOnOpenKeepsParkedRead();
        testAssignDoesNotTouchFlagsButReadDoes();
        testWaitDoesNotTouchFlags();
        testWaitReadParksUntilDataArrives();
        testWaitWriteOnFullPipe();
        testWriteSome();
        testCancelPendingRead();
        testReleaseCancelsAndTransfersOwnership();
        testEofOnClosedWriteEnd();
        testParkedReadSeesEofWhenWriterCloses();
        testWaitReadOnHungUpPipeIsReadiness();
        testGatherReadWrite();
        testReassignRearmsNonblockingLatch();
        testParkedWriteOnBrokenPipeReportsEpipe();
        testWaitErrorParksThenNamesRealCode();
        testWaitOnClosedDescriptor();
        testConcurrentFirstReadAndWrite();
        testUnpollableCharDeviceAdoptsEverywhere();
        testUnpollableWaitThatWouldParkIsNotSupported();
        testSelectFdSetsizeIsRejected();
        testWaitErrorOnHungUpReadEnd();
        testWaitErrorParksOnReadEndThenNamesCode();
        testSelectDoesNotSpinOnReadableIdleFd();
        testWriteSomeArmsNonblocking();
        testStopTokenCancelsRead();
        testZeroLength();
        testReleaseWhenClosedThrows();
        testReleaseThenReadoptElsewhere();
        testComposedRead();
        testAdoptPipeWhosePeerIsGone();
        testWaitWriteWithReaderGoneIsReadiness();
        testReaderlessPipeAdoptsAndWriteFails();
        testKqueueFdAdoptsWithoutWriteFilter();
        testBlockingFdReadIsCancellable();
    }
};

COROSIO_BACKEND_TESTS(
    posix_stream_descriptor_test, "boost.corosio.posix_stream_descriptor")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
