//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// io_uring scheduler paths that need contended, multi-threaded dispatch: a
// follower parked in cond_.wait_for while another thread holds the ring
// leadership, woken by work posted from a foreign thread. Durations only
// bound blocking; nothing asserts elapsed time.

#include "must.hpp"
#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/delay.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/detail/uring/uring_socket_ops.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stop_token>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio {

// A poll on a pipe that never becomes readable, so only a cancel can
// complete it.
struct cancel_probe_op : detail::uring_op
{
    int fd = -1;
    std::atomic<bool> done{false};
    int result = 0;

    cancel_probe_op() noexcept : uring_op(&on_done, &on_cqe, &prep) {}

    static void prep(detail::uring_op* base, ::io_uring_sqe* sqe) noexcept
    {
        ::io_uring_prep_poll_add(
            sqe, static_cast<cancel_probe_op*>(base)->fd, POLLIN);
    }

    static void on_cqe(
        detail::uring_op* base,
        int res,
        unsigned /*flags*/,
        detail::ready_queue& local) noexcept
    {
        base->res = res;
        local.push(base);
    }

    static void on_done(
        void* /*owner*/,
        detail::scheduler_op* base,
        std::uint32_t /*bytes*/,
        std::uint32_t /*error*/) noexcept
    {
        auto* self   = static_cast<cancel_probe_op*>(base);
        self->result = self->res;
        self->done   = true;
    }
};

struct uring_probe_context : native_io_context<uring>
{
    using native_io_context<uring>::native_io_context;

    detail::uring_scheduler& scheduler() noexcept
    {
        return *static_cast<detail::uring_scheduler*>(sched_);
    }
};

struct uring_mt_scheduler_test
{
    void testForeignPostWakesParkedFollower()
    {
        native_io_context<uring> ioc;
        auto ex = ioc.get_executor();
        auto [s1, s2] = test::must(
            test::make_socket_pair<tcp_socket, tcp_acceptor, false>(ioc));

        // A parked read keeps outstanding work alive so both runner threads
        // stay inside the scheduler: one leads the ring, the other parks in
        // cond_.wait_for. Each foreign post then has a follower to wake.
        // The closure is a named local (not a temporary), so the suspended
        // coroutine's captures outlive it.
        char buf[4];
        bool resumed = false;
        auto reader  = [&]() -> capy::task<> {
            auto [ec, n] =
                co_await s1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            std::ignore = ec;
            std::ignore = n;
            resumed     = true;
        };
        capy::run_async(ex)(reader());

        std::atomic<int> entered{0};
        std::atomic<int> nops{0};
        // A fixed slice count (rather than a work-gated one) keeps both
        // threads in the scheduler through quiet windows: with the read
        // parked, one thread leads the ring while the other parks in
        // cond_.wait_for and times out, then they trade roles.
        auto slice = [&] {
            entered.fetch_add(1);
            for (int i = 0; i < 8; ++i)
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(5));
        };
        std::thread ra(slice), rb(slice);

        while (entered.load() < 2)
        {
        }
        for (int i = 0; i < 20; ++i)
        {
            // The counter travels as a parameter: a loop-scoped closure
            // would die before the runner threads execute the frame that
            // references it.
            capy::run_async(ex)([](std::atomic<int>* n) -> capy::task<> {
                n->fetch_add(1);
                co_return;
            }(&nops));
        }
        ra.join();
        rb.join();

        // Drain any posts the fixed slices did not reach, plus the read.
        s1.cancel();
        ioc.restart();
        ioc.run();
        BOOST_TEST_EQ(nops.load(), 20);
        BOOST_TEST(resumed);
    }

    // unsafe_io keeps the scheduler lock, so a foreign thread may post;
    // the run thread blocked in the kernel must wake for it rather than
    // sleep until the pending hour-long timer expires.
    void testForeignPostWakesUnsafeIoRun()
    {
        io_context_options opts;
        opts.locking = locking_mode::unsafe_io;
        native_io_context<uring> ioc(opts, 1);
        auto ex = ioc.get_executor();

        std::stop_source keep;
        capy::run_async(ex, keep.get_token())([]() -> capy::task<> {
            std::ignore = co_await delay(std::chrono::hours(1));
        }());

        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            for (int i = 0; i < 300 && !finished.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!finished.load())
            {
                std::fputs("unsafe_io: foreign post never woke run()\n", stderr);
                std::abort();
            }
        });
        std::thread poster([&] {
            // Lets run() block in the kernel first; a post that lands
            // earlier only lets the test pass without exercising the wake.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            capy::run_async(ex)([](std::stop_source& k) -> capy::task<> {
                k.request_stop();
                co_return;
            }(keep));
        });

        ioc.run();
        finished = true;
        poster.join();
        watchdog.join();
    }

    // With a kernel thread polling the submission queue, submitting a
    // cancel only hands it over. Closing the descriptor before that
    // thread resolves the number would leave the read armed for good.
    void testSqpollCloseCancelsParkedRead()
    {
        io_context_options opts;
        opts.enable_sqpoll     = true;
        opts.sq_thread_idle_ms = 1000;
        native_io_context<uring> ioc(opts, 1);
        int stranded = 0;
        for (int i = 0; i < 50; ++i)
        {
            int sv[2];
            BOOST_TEST(
                ::socketpair(
                    AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                    sv) == 0);
            local_stream_socket s(ioc);
            BOOST_TEST(!s.assign(sv[0]));
            bool done = false;
            std::error_code rec;
            char buf[8];
            capy::run_async(ioc.get_executor())(
                [](local_stream_socket& s, char* b, bool& done,
                   std::error_code& ec) -> capy::task<> {
                    auto [e, n] =
                        co_await s.read_some(capy::mutable_buffer(b, 8));
                    std::ignore = n;
                    ec          = e;
                    done        = true;
                }(s, buf, done, rec));
            ioc.restart();
            std::ignore = ioc.poll();
            // Lets the kernel thread take the read, so it is armed.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            s.close();
            ioc.restart();
            // Bounded only to fail rather than hang.
            for (int k = 0; k < 50 && !done; ++k)
                std::ignore = ioc.run_one_for(std::chrono::milliseconds(20));
            if (!done || rec != capy::cond::canceled)
                ++stranded;
            ::close(sv[1]);
            ioc.restart();
            while (!done && ioc.run_one_for(std::chrono::seconds(1)) != 0)
            {
            }
        }
        BOOST_TEST_EQ(stranded, 0);
    }

    // A stop that lands after an initiator found the op uncancelled,
    // but before its SQE is linked, finds nothing to cancel yet; the
    // link must cancel the op itself.
    void testStopBeforeLinkCancelsOp()
    {
        uring_probe_context ctx;
        auto& sched = ctx.scheduler();
        int p[2];
        BOOST_TEST_EQ(::pipe2(p, O_NONBLOCK | O_CLOEXEC), 0);

        cancel_probe_op op;
        op.fd     = p[0];
        op.sched_ = &sched;
        std::stop_source src;
        op.start(src.get_token());
        sched.work_started();

        src.request_stop();
        detail::uring_submit_op(sched, &op);

        // Bounds a hang, not a timing assertion.
        while (!op.done && ctx.run_one_for(std::chrono::seconds(5)) != 0)
        {
        }
        BOOST_TEST(op.done);
        BOOST_TEST_EQ(op.result, -ECANCELED);

        if (!op.done)
        {
            std::ignore = ::write(p[1], "x", 1);
            ctx.restart();
            ctx.run();
        }
        ::close(p[0]);
        ::close(p[1]);
    }

    // A ring that defers completion work to its submitting thread is
    // only usable when that work still reaches a thread asleep outside
    // the ring.
    void testRingDefersCompletionWorkWhenSupported()
    {
        ::io_uring probe{};
        io_uring_params p{};
        unsigned const taskrun =
            IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
        p.flags              = IORING_SETUP_SUBMIT_ALL | taskrun;
        bool const supported = ::io_uring_queue_init_params(8, &probe, &p) == 0;
        if (supported)
            ::io_uring_queue_exit(&probe);

        uring_probe_context plain;
        BOOST_TEST_EQ(
            plain.scheduler().ring()->flags & taskrun,
            supported ? taskrun : 0u);

        // The SQPOLL ring refuses the flags; it must keep SUBMIT_ALL.
        io_context_options opts;
        opts.enable_sqpoll = true;
        uring_probe_context polled(opts, 1);
        auto const flags = polled.scheduler().ring()->flags;
        BOOST_TEST_EQ(flags & taskrun, 0u);
        if (supported)
            BOOST_TEST_NE(flags & IORING_SETUP_SUBMIT_ALL, 0u);
    }

    // This thread submits the read, then sleeps in a join while another
    // thread leads the ring. The kernel finishes the read on the
    // submitting thread, so the leader sees it only if that thread is
    // woken to do so.
    void testSleepingSubmitterStillCompletes()
    {
        uring_probe_context ioc;
        int sv[2];
        BOOST_TEST(
            ::socketpair(
                AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) ==
            0);
        local_stream_socket s(ioc);
        BOOST_TEST(!s.assign(sv[0]));

        char buf     = 0;
        bool resumed = false;
        capy::run_async(ioc.get_executor())(
            [](local_stream_socket& s, char* b, bool& resumed) -> capy::task<> {
                auto [ec, n] = co_await s.read_some(capy::mutable_buffer(b, 1));
                std::ignore  = ec;
                std::ignore  = n;
                resumed      = true;
            }(s, &buf, resumed));
        std::ignore = ioc.poll();
        BOOST_TEST(!resumed);
        BOOST_TEST_EQ(::io_uring_sq_ready(ioc.scheduler().ring()), 0u);

        std::atomic<bool> finished{false};
        std::thread watchdog([&] {
            for (int i = 0; i < 300 && !finished.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!finished.load())
            {
                std::fputs(
                    "sleeping submitter: read never completed\n", stderr);
                std::abort();
            }
        });
        std::thread leader([&] { ioc.run(); });
        std::thread writer([&] {
            // Lets this thread fall asleep in the join first; an earlier
            // write only lets the test pass without exercising the wake.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::ignore = ::write(sv[1], "x", 1);
        });
        writer.join();
        leader.join();
        finished = true;
        watchdog.join();

        BOOST_TEST(resumed);
        BOOST_TEST_EQ(buf, 'x');
        ::close(sv[1]);
    }

    // Completion work owed to this thread waits for its next kernel
    // entry. A pass that only peeks must still finish it while handlers
    // keep the thread in user space.
    void testPeekingPassFindsWaitingCompletion()
    {
        uring_probe_context ioc;
        int sv[2];
        BOOST_TEST(
            ::socketpair(
                AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) ==
            0);
        local_stream_socket s(ioc);
        BOOST_TEST(!s.assign(sv[0]));

        char buf     = 0;
        bool resumed = false;
        capy::run_async(ioc.get_executor())(
            [](local_stream_socket& s, char* b, bool& resumed) -> capy::task<> {
                auto [ec, n] = co_await s.read_some(capy::mutable_buffer(b, 1));
                std::ignore  = ec;
                std::ignore  = n;
                resumed      = true;
            }(s, &buf, resumed));
        std::ignore = ioc.poll();
        BOOST_TEST(!resumed);
        BOOST_TEST_EQ(::io_uring_sq_ready(ioc.scheduler().ring()), 0u);

        std::atomic<bool> written{false};
        std::thread writer([&] {
            std::ignore = ::write(sv[1], "x", 1);
            written.store(true);
        });
        // Spins rather than blocks: any kernel entry here would finish
        // the work before the pass gets to.
        while (!written.load())
        {
        }
        std::ignore = ioc.poll();
        BOOST_TEST(resumed);
        BOOST_TEST_EQ(buf, 'x');

        writer.join();
        if (!resumed)
        {
            ioc.restart();
            ioc.run();
        }
        ::close(sv[1]);
    }

    void run()
    {
        testRingDefersCompletionWorkWhenSupported();
        testPeekingPassFindsWaitingCompletion();
        testSleepingSubmitterStillCompletes();
        testStopBeforeLinkCancelsOp();
        testForeignPostWakesParkedFollower();
        testForeignPostWakesUnsafeIoRun();
        testSqpollCloseCancelsParkedRead();
    }
};

TEST_SUITE(uring_mt_scheduler_test, "boost.corosio.native.uring.mt_scheduler");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
