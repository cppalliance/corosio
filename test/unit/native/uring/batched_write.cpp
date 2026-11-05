//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "must.hpp"
#include "test_suite.hpp"

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_URING

#include <boost/corosio/backend.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <thread>

// The tasks take their state through by-value parameters (pointers to
// test-scope objects), never by lambda capture: a captured closure is a
// temporary destroyed at the end of the run_async statement.

namespace boost::corosio {

namespace {

struct batch_test_context : native_io_context<uring>
{
    detail::uring_scheduler& uring_sched()
    {
        return *static_cast<detail::uring_scheduler*>(sched_);
    }
};

struct io_result
{
    std::error_code ec;
    std::size_t n = 0;
};

capy::task<>
read_one(tcp_socket* s, char* buf, io_result* out)
{
    auto [ec, n] = co_await s->read_some(capy::mutable_buffer(buf, 1));
    out->ec      = ec;
    out->n       = n;
}

capy::task<>
write_one(tcp_socket* s, char const* c, io_result* out)
{
    auto [ec, n] = co_await s->write_some(capy::const_buffer(c, 1));
    out->ec      = ec;
    out->n       = n;
}

struct batch_observations
{
    bool may_join            = false;
    std::int64_t after_read  = 0;
    std::int64_t after_write = 0;
};

// Starts both tasks from one handler, so the read's queued SQE is still
// waiting for its batch submit when the write is initiated.
capy::task<>
read_then_write(
    batch_test_context* ctx,
    tcp_socket* reader,
    tcp_socket* writer,
    char* buf,
    char const* c,
    io_result* rd,
    io_result* wr,
    batch_observations* seen)
{
    auto ex = ctx->get_executor();
    capy::run_async(ex)(read_one(reader, buf, rd));
    seen->may_join   = ctx->uring_sched().write_may_join_batch();
    seen->after_read = ctx->uring_sched().inflight();
    capy::run_async(ex)(write_one(writer, c, wr));
    seen->after_write = ctx->uring_sched().inflight();
    co_return;
}

// Parks one runner, so the queued batch cannot run on it while the
// other initiates the write.
capy::task<>
park(std::atomic<bool>* parked, std::atomic<bool>* release)
{
    parked->store(true);
    while (!release->load())
        std::this_thread::yield();
    co_return;
}

capy::task<>
read_then_write_two_runners(
    batch_test_context* ctx,
    tcp_socket* reader,
    tcp_socket* writer,
    char* buf,
    char const* c,
    io_result* rd,
    io_result* wr,
    batch_observations* seen,
    std::atomic<bool>* parked,
    std::atomic<bool>* release)
{
    while (!parked->load())
        std::this_thread::yield();
    co_await read_then_write(ctx, reader, writer, buf, c, rd, wr, seen);
    release->store(true);
}

capy::task<>
write_alone(
    batch_test_context* ctx,
    tcp_socket* writer,
    char const* c,
    io_result* wr,
    batch_observations* seen)
{
    seen->may_join = ctx->uring_sched().write_may_join_batch();
    capy::run_async(ctx->get_executor())(write_one(writer, c, wr));
    seen->after_write = ctx->uring_sched().inflight();
    co_return;
}

} // namespace

struct batched_write_test
{
    // A write initiated while a batch submit is queued joins the batch
    // through the ring instead of being issued directly, and still
    // delivers its data.
    void testWriteJoinsQueuedBatch()
    {
        batch_test_context ctx;
        auto [a, b] = test::must(test::make_socket_pair(ctx));

        char buf     = 0;
        char const c = 'x';
        io_result rd, wr;
        batch_observations seen;

        capy::run_async(ctx.get_executor())(
            read_then_write(&ctx, &b, &a, &buf, &c, &rd, &wr, &seen));
        ctx.run();

        BOOST_TEST(seen.may_join);
        BOOST_TEST_EQ(seen.after_write, seen.after_read + 1);
        BOOST_TEST(!wr.ec);
        BOOST_TEST_EQ(wr.n, 1u);
        BOOST_TEST(!rd.ec);
        BOOST_TEST_EQ(rd.n, 1u);
        BOOST_TEST_EQ(buf, 'x');
        BOOST_TEST_EQ(ctx.uring_sched().inflight(), 0);
    }

    // With two threads running the scheduler the batch may be
    // submitted from either, so the write is issued directly.
    void testWriteWithTwoRunnersIsDirect()
    {
        batch_test_context ctx;
        auto [a, b] = test::must(test::make_socket_pair(ctx));

        char buf     = 0;
        char const c = 'z';
        io_result rd, wr;
        batch_observations seen;

        std::atomic<bool> parked{false};
        std::atomic<bool> release{false};

        capy::run_async(ctx.get_executor())(park(&parked, &release));
        capy::run_async(ctx.get_executor())(read_then_write_two_runners(
            &ctx, &b, &a, &buf, &c, &rd, &wr, &seen, &parked, &release));
        std::thread other([&] { ctx.run(); });
        ctx.run();
        other.join();

        BOOST_TEST(!seen.may_join);
        BOOST_TEST_EQ(seen.after_write, seen.after_read);
        BOOST_TEST(!wr.ec);
        BOOST_TEST_EQ(wr.n, 1u);
        BOOST_TEST(!rd.ec);
        BOOST_TEST_EQ(rd.n, 1u);
        BOOST_TEST_EQ(buf, 'z');
    }

    // With no batch queued, a write is issued directly and completes
    // without an SQE.
    void testWriteWithoutBatchIsDirect()
    {
        batch_test_context ctx;
        auto [a, b] = test::must(test::make_socket_pair(ctx));

        char const c = 'y';
        io_result wr;
        batch_observations seen;

        capy::run_async(ctx.get_executor())(
            write_alone(&ctx, &a, &c, &wr, &seen));
        ctx.run();

        BOOST_TEST(!seen.may_join);
        BOOST_TEST_EQ(seen.after_write, 0);
        BOOST_TEST(!wr.ec);
        BOOST_TEST_EQ(wr.n, 1u);
    }

    void run()
    {
        testWriteJoinsQueuedBatch();
        testWriteWithTwoRunnersIsDirect();
        testWriteWithoutBatchIsDirect();
    }
};

TEST_SUITE(batched_write_test, "boost.corosio.native.uring.batched_write");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_URING
