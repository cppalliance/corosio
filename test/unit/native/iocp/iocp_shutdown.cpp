//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_op.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include "test_suite.hpp"

namespace boost::corosio {

// Test helper: exposes IOCP handle for direct posting.
struct iocp_test_context : native_io_context<iocp>
{
    void* iocp_handle()
    {
        return static_cast<detail::win_scheduler*>(sched_)->native_handle();
    }
};

// Overlapped op that increments a counter when destroyed during shutdown.
struct test_overlapped_op : detail::overlapped_op
{
    int* destroyed_;

    static void do_complete(
        void* owner, detail::scheduler_op* base, std::uint32_t, std::uint32_t)
    {
        auto* self = static_cast<test_overlapped_op*>(base);
        if (!owner)
        {
            ++(*self->destroyed_);
            delete self;
            return;
        }
        delete self;
    }

    explicit test_overlapped_op(int& destroyed)
        : detail::overlapped_op(&do_complete)
        , destroyed_(&destroyed)
    {
    }
};

struct iocp_shutdown_test
{
    // Shutdown drains I/O completions (key_io) from the IOCP.
    // Covers the else branch of `if (key == key_posted)` in shutdown().
    void testShutdownDrainsIOCompletion()
    {
        int destroyed = 0;

        {
            iocp_test_context ctx;
            auto ex   = ctx.get_executor();
            void* ioc = ctx.iocp_handle();

            auto* op = new test_overlapped_op(destroyed);

            ex.on_work_started();

            BOOL ok = ::PostQueuedCompletionStatus(
                ioc, 0, detail::key_io, static_cast<LPOVERLAPPED>(op));
            BOOST_TEST(ok != 0);
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    // Shutdown drains key_result_stored completions from the IOCP.
    void testShutdownDrainsResultStoredCompletion()
    {
        int destroyed = 0;

        {
            iocp_test_context ctx;
            auto ex   = ctx.get_executor();
            void* ioc = ctx.iocp_handle();

            auto* op              = new test_overlapped_op(destroyed);
            op->ready_            = 1;
            op->dwError           = 0;
            op->bytes_transferred = 42;

            ex.on_work_started();

            BOOL ok = ::PostQueuedCompletionStatus(
                ioc, 0, detail::key_result_stored,
                static_cast<LPOVERLAPPED>(op));
            BOOST_TEST(ok != 0);
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    // Shutdown drains multiple I/O completions with different keys.
    void testShutdownDrainsMixedCompletions()
    {
        int io_destroyed     = 0;
        int stored_destroyed = 0;

        {
            iocp_test_context ctx;
            auto ex   = ctx.get_executor();
            void* ioc = ctx.iocp_handle();

            auto* io_op = new test_overlapped_op(io_destroyed);
            ex.on_work_started();
            ::PostQueuedCompletionStatus(
                ioc, 0, detail::key_io, static_cast<LPOVERLAPPED>(io_op));

            auto* stored_op    = new test_overlapped_op(stored_destroyed);
            stored_op->ready_  = 1;
            stored_op->dwError = 0;
            stored_op->bytes_transferred = 0;
            ex.on_work_started();
            ::PostQueuedCompletionStatus(
                ioc, 0, detail::key_result_stored,
                static_cast<LPOVERLAPPED>(stored_op));
        }

        BOOST_TEST_EQ(io_destroyed, 1);
        BOOST_TEST_EQ(stored_destroyed, 1);
    }

    void run()
    {
        testShutdownDrainsIOCompletion();
        testShutdownDrainsResultStoredCompletion();
        testShutdownDrainsMixedCompletions();
    }
};

TEST_SUITE(iocp_shutdown_test, "boost.corosio.native.iocp_shutdown");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
