//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// The two-phase transfer machinery in uring_descriptor: a kernel EAGAIN
// arms a poll_add, the poll's completion re-submits the transfer, and
// every path that abandons the op in between leaves a terminal result
// behind. None of this is reachable through posix_descriptor's public
// API: io_uring retries a pollable O_NONBLOCK descriptor internally, so
// -EAGAIN never reaches userspace for the descriptor kinds this type
// carries, and the remaining producer (an SQ ring that stays full after
// a flush) needs a CQ overflow that a unit test cannot force on a
// 256-entry ring. The CQE is therefore injected here and the rest of
// the path -- the real op types, the real ring, a real pipe -- runs
// unmodified.

#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/detail/uring/uring_descriptor.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>

#include <coroutine>
#include <cstring>
#include <memory>
#include <system_error>

#include <errno.h>
#include <poll.h>
#include <unistd.h>

namespace boost::corosio {

// Exposes the io_uring scheduler, the way multishot_acceptor.cpp does.
struct uring_descriptor_test_context : native_io_context<uring>
{
    detail::uring_scheduler& scheduler() noexcept
    {
        return *static_cast<detail::uring_scheduler*>(sched_);
    }
};

// A real coroutine handle for the completing op to resume. The body runs
// on the first resume, because initial_suspend suspends.
struct probe_coro
{
    struct promise_type
    {
        probe_coro get_return_object() noexcept
        {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept
        {
            return {};
        }
        std::suspend_always final_suspend() noexcept
        {
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };

    std::coroutine_handle<promise_type> h;
};

inline probe_coro
make_probe_coro(bool& resumed)
{
    resumed = true;
    co_return;
}

struct uring_descriptor_continue_test
{
    // What uring_descriptor::arm_slot does at submission time; the
    // epochs it snapshots are the whole subject of these tests.
    template<class Op>
    static void arm(detail::uring_descriptor& d, Op& op)
    {
        op.desc         = &d;
        op.polling      = false;
        op.cancel_epoch = d.cancel_epoch();
        op.desc_epoch   = d.desc_epoch();
    }

    // The handler's tail, so an assertion reads the same two values a
    // caller of read_some() would.
    static void
    decode(detail::uring_op& op, std::error_code& ec, std::size_t& bytes)
    {
        op.ec_out = &ec;
        detail::uring_set_result(&op, /*is_read=*/true, op.empty_buffer);
        bytes = op.res >= 0 ? static_cast<std::size_t>(op.res) : 0u;
    }

    void testCancelledMidPollDoesNotReportRevents()
    {
        // A stop_token firing between a successful poll CQE and its
        // dispatch. `res` still holds the revents mask, which the
        // completion decode would read as a byte count.
        uring_descriptor_test_context ctx;
        auto d = std::make_shared<detail::uring_descriptor>(ctx.scheduler());

        detail::uring_descriptor_read_op op;
        arm(*d, op);
        op.polling = true;
        op.res     = POLLIN;
        op.cancelled.store(true, std::memory_order_release);

        BOOST_TEST(!detail::uring_descriptor_continue(op));
        BOOST_TEST_EQ(op.res, -ECANCELED);

        std::error_code ec;
        std::size_t bytes = 99;
        decode(op, ec, bytes);
        BOOST_TEST(ec == capy::cond::canceled);
        // Without the terminal result this is POLLIN, i.e. one byte the
        // caller never read.
        BOOST_TEST_EQ(bytes, 0u);
    }

    void testCancelInTheGapStopsTheRearm()
    {
        // cancel() lands while the op is out of the ring, so its
        // cancel-by-fd SQE finds nothing and the op's own `cancelled`
        // flag is never set. The epoch is the only thing that sees it.
        uring_descriptor_test_context ctx;
        auto d = std::make_shared<detail::uring_descriptor>(ctx.scheduler());

        detail::uring_descriptor_write_op op;
        arm(*d, op);
        op.polling = true;
        op.res     = POLLOUT;

        BOOST_TEST(!op.cancelled.load(std::memory_order_acquire));
        d->cancel();
        BOOST_TEST(!op.cancelled.load(std::memory_order_acquire));

        BOOST_TEST(!detail::uring_descriptor_continue(op));
        BOOST_TEST_EQ(op.res, -ECANCELED);
    }

    void testEagainCancelInTheGapDoesNotPark()
    {
        // The hang the epoch prevents: a transfer that answered EAGAIN
        // would otherwise arm a fresh poll nothing can cancel, and
        // run() would never return.
        uring_descriptor_test_context ctx;
        auto d = std::make_shared<detail::uring_descriptor>(ctx.scheduler());

        detail::uring_descriptor_read_op op;
        arm(*d, op);
        op.res = -EAGAIN;
        d->cancel();

        BOOST_TEST(!detail::uring_descriptor_continue(op));
        // Still the transfer phase: nothing was submitted, so nothing
        // is parked.
        BOOST_TEST(!op.polling);
        BOOST_TEST_EQ(op.res, -EAGAIN);
    }

    void testRecycledFdNumberStopsTheRearm()
    {
        // The aliasing case a bare native_handle() == op.fd comparison
        // cannot see: the descriptor is re-adopted and lands on the
        // same fd number.
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);

        uring_descriptor_test_context ctx;
        auto d = std::make_shared<detail::uring_descriptor>(ctx.scheduler());
        d->set_descriptor(fds[0]);

        detail::uring_descriptor_read_op op;
        arm(*d, op);
        op.fd      = fds[0];
        op.polling = true;
        op.res     = POLLIN;

        d->set_descriptor(fds[0]);
        BOOST_TEST_EQ(d->native_handle(), op.fd); // the fd check would pass

        BOOST_TEST(!detail::uring_descriptor_continue(op));
        BOOST_TEST_EQ(op.res, -EBADF);

        std::error_code ec;
        std::size_t bytes = 99;
        decode(op, ec, bytes);
        // Without the terminal result: no error at all, and a
        // one-byte read of an untouched buffer.
        BOOST_TEST(ec == std::errc::bad_file_descriptor);
        BOOST_TEST_EQ(bytes, 0u);

        ::close(fds[1]);
    }

    void testTerminalResultsAreLeftAlone()
    {
        uring_descriptor_test_context ctx;
        auto d = std::make_shared<detail::uring_descriptor>(ctx.scheduler());

        // A poll that failed or was cancelled is already the answer.
        detail::uring_descriptor_read_op poll_failed;
        arm(*d, poll_failed);
        poll_failed.polling = true;
        poll_failed.res     = -ECANCELED;
        BOOST_TEST(!detail::uring_descriptor_continue(poll_failed));
        BOOST_TEST_EQ(poll_failed.res, -ECANCELED);

        // A transfer error is not a readiness mask; leave it.
        detail::uring_descriptor_write_op failed;
        arm(*d, failed);
        failed.res = -EPIPE;
        BOOST_TEST(!detail::uring_descriptor_continue(failed));
        BOOST_TEST_EQ(failed.res, -EPIPE);

        // Nor is a genuine byte count.
        detail::uring_descriptor_read_op transferred;
        arm(*d, transferred);
        transferred.res = 5;
        BOOST_TEST(!detail::uring_descriptor_continue(transferred));
        BOOST_TEST_EQ(transferred.res, 5);
    }

    void testEagainArmsPollThenRetriesTransfer()
    {
        // The forward path, end to end on a real ring: injected EAGAIN
        // -> poll_add -> readiness -> re-submitted READV -> coroutine
        // resumed with the bytes.
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);

        uring_descriptor_test_context ctx;
        auto ex = ctx.get_executor();
        auto d  = std::make_shared<detail::uring_descriptor>(ctx.scheduler());
        d->set_descriptor(fds[0]);

        bool resumed = false;
        auto coro    = make_probe_coro(resumed);

        char buf[8]{};
        capy::mutable_buffer mb(buf, sizeof(buf));
        std::error_code ec;
        std::size_t bytes = 0;

        detail::uring_descriptor_read_op op;
        op.prepare(
            coro.h, ex, &ec, &bytes, fds[0], /*file_offset=*/-1,
            &ctx.scheduler(), d, mb, std::stop_token{});
        arm(*d, op);

        // Data is already waiting, so the poll this arms fires at once.
        BOOST_TEST_EQ(::write(fds[1], "hi", 2), 2);

        op.res = -EAGAIN;
        BOOST_TEST(detail::uring_descriptor_continue(op));
        BOOST_TEST(op.polling); // phase flipped; a poll_add is in the ring

        ctx.run();

        BOOST_TEST(resumed);
        BOOST_TEST(!op.polling); // the poll's CQE re-submitted the read
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(bytes, 2u);
        BOOST_TEST_EQ(std::memcmp(buf, "hi", 2), 0);

        coro.h.destroy();
        ::close(fds[1]);
    }

    void run()
    {
        testCancelledMidPollDoesNotReportRevents();
        testCancelInTheGapStopsTheRearm();
        testEagainCancelInTheGapDoesNotPark();
        testRecycledFdNumberStopsTheRearm();
        testTerminalResultsAreLeftAlone();
        testEagainArmsPollThenRetriesTransfer();
    }
};

TEST_SUITE(
    uring_descriptor_continue_test,
    "boost.corosio.native.uring.descriptor_continue");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
