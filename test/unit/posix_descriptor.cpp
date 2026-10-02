//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/posix_descriptor.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/delay.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <array>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <typeinfo>

#include <fcntl.h>
#include <unistd.h>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

static_assert(capy::ReadStream<posix_descriptor>);
static_assert(capy::WriteStream<posix_descriptor>);
static_assert(std::is_base_of_v<io_stream, posix_descriptor>);
static_assert(!std::is_copy_constructible_v<posix_descriptor>);
static_assert(std::is_move_constructible_v<posix_descriptor>);

// assign() must be impossible to call and ignore.
static_assert(std::is_same_v<
              decltype(std::declval<posix_descriptor&>().assign(
                  std::declval<native_handle_type>())),
              std::error_code>);

struct posix_descriptor_contract_test
{
    void run() {}
};

TEST_SUITE(
    posix_descriptor_contract_test, "boost.corosio.posix_descriptor_contract");

template<auto Backend>
struct posix_descriptor_test
{
    // Returns a pipe whose ends are both blocking, so every test that
    // cares about O_NONBLOCK starts from a known state.
    static void make_pipe(int (&fds)[2])
    {
        BOOST_TEST_EQ(::pipe(fds), 0);
    }

    // The .epoll, .select and .uring variants are separate ctest
    // entries that run concurrently under ctest -j, so a fixed global
    // name would race between them.
    static std::filesystem::path temp_path(char const* tag)
    {
        return std::filesystem::temp_directory_path() /
            ("corosio_posix_descriptor_" + std::string(tag) + "_" +
             typeid(decltype(Backend)).name() + "_" +
             std::to_string(::getpid()));
    }

    void testConstruction()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);
        BOOST_TEST_EQ(d.is_open(), false);
        BOOST_TEST_EQ(d.native_handle(), -1);
    }

    void testAssignRejectsRegularFile()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);

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
        posix_descriptor d(ioc);
        BOOST_TEST(d.assign(-1) == std::errc::bad_file_descriptor);
    }

    void testAssignRejectsSelf()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);

        BOOST_TEST(!d.assign(fds[0]));
        BOOST_TEST(d.assign(d.native_handle()) == std::errc::invalid_argument);
        // Still open on the same fd after the rejected self-assign.
        BOOST_TEST_EQ(d.is_open(), true);
        BOOST_TEST_EQ(d.native_handle(), fds[0]);

        ::close(fds[1]);
    }

    void testFailedAssignLeavesPriorStateIntact()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[0]));

        auto path = temp_path("reject");
        int reg   = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        BOOST_TEST(reg >= 0);

        BOOST_TEST(d.assign(reg) == std::errc::operation_not_supported);

        // The held descriptor survived the rejected assign and still works.
        BOOST_TEST_EQ(d.native_handle(), fds[0]);
        char const msg[] = "ok";
        BOOST_TEST_EQ(::write(fds[1], msg, 2), 2);

        auto ex = ioc.get_executor();
        char buf[8]{};
        std::size_t n = 0;
        std::error_code ec;
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = rec;
            n  = rn;
        };
        capy::run_async(ex)(reader());
        ioc.run();

        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);

        ::close(reg);
        std::filesystem::remove(path);
        ::close(fds[1]);
    }

    void testAssignDoesNotTouchFlagsButReadDoes()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);
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
        // ...and the first read is what flipped the flag.
        BOOST_TEST(::fcntl(fds[0], F_GETFL) & O_NONBLOCK);

        ::close(fds[1]);
    }

    void testWaitDoesNotTouchFlags()
    {
        io_context ioc(Backend);
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
        int fds[2];
        make_pipe(fds);
        BOOST_TEST(!d.assign(fds[1]));

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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
            posix_descriptor d(ioc);
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
        posix_descriptor w(ioc);
        posix_descriptor r(ioc);
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
        posix_descriptor d(ioc);
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
            BOOST_TEST(::fcntl(first[0], F_GETFL) & O_NONBLOCK);

            // Adopting over the armed descriptor must not inherit its
            // latch; the fresh pipe end is still blocking.
            BOOST_TEST(!d.assign(second[0]));
            BOOST_TEST_EQ(::fcntl(second[0], F_GETFL) & O_NONBLOCK, 0);

            BOOST_TEST_EQ(::write(second[1], "xy", 2), 2);
            auto [ec2, n2] =
                co_await d.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            BOOST_TEST(!ec2);
            BOOST_TEST_EQ(n2, 2u);
            BOOST_TEST_EQ(std::memcmp(buf, "xy", 2), 0);
            BOOST_TEST(::fcntl(second[0], F_GETFL) & O_NONBLOCK);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);
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
        posix_descriptor d(ioc);

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

    void run()
    {
        testConstruction();
        testAssignRejectsRegularFile();
        testAssignRejectsBadFd();
        testAssignRejectsSelf();
        testFailedAssignLeavesPriorStateIntact();
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
    }
};

COROSIO_BACKEND_TESTS(posix_descriptor_test, "boost.corosio.posix_descriptor")

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
