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

#include <boost/corosio/backend.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/win_random_access_handle.hpp>
#include <boost/corosio/win_stream_handle.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <tuple>
#include <vector>

#include "../../win_test_handles.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Destroying the context with a read still pending on a handle must
// drain the operation without resuming the coroutine.
struct iocp_handle_teardown_test
{
    void testDestroyWithPendingStreamHandleRead()
    {
        auto p       = test::make_pipe_pair();
        bool resumed = false;
        {
            io_context ioc(iocp);
            auto reader = [&]() -> capy::task<> {
                win_stream_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(p.server.release())));
                char buf[16];
                std::ignore = co_await h.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyWithPendingRandomAccessHandleRead()
    {
        auto p       = test::make_pipe_pair();
        bool resumed = false;
        {
            io_context ioc(iocp);
            auto reader = [&]() -> capy::task<> {
                win_random_access_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(p.server.release())));
                char buf[16];
                std::ignore = co_await h.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyWithPendingStreamHandleWrite()
    {
        // The client end never reads, so a write larger than the pipe's
        // 4 KiB buffer stays pending.
        auto p       = test::make_pipe_pair();
        bool resumed = false;
        std::vector<char> big(64 * 1024, 'x');
        {
            io_context ioc(iocp);
            auto writer = [&]() -> capy::task<> {
                win_stream_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(p.server.release())));
                std::ignore = co_await h.write_some(
                    capy::const_buffer(big.data(), big.size()));
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(writer());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyDrainsBothEndsOfOnePipe()
    {
        // Both ends adopted, each with a read pending: shutdown closes
        // one end while the other's read is still pending, which fails
        // that read with a broken pipe during the drain. A write on one
        // end would satisfy the other's read, so both ops are reads.
        auto const name =
            L"\\\\.\\pipe\\corosio_test_" + test::unique_suffix();
        test::unique_handle server(::CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
            0, nullptr));
        test::unique_handle client(::CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        BOOST_TEST(server && client);

        int resumed = 0;
        {
            io_context ioc(iocp);
            auto reader = [&](test::unique_handle& end) -> capy::task<> {
                win_stream_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(end.release())));
                char buf[16];
                std::ignore = co_await h.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                ++resumed;
            };
            capy::run_async(ioc.get_executor())(reader(server));
            capy::run_async(ioc.get_executor())(reader(client));
            std::ignore = ioc.run_one();
            std::ignore = ioc.run_one();
        }
        BOOST_TEST_EQ(resumed, 0);
    }

    void testDestroyWithSeveralPendingRandomAccessReads()
    {
        auto p      = test::make_pipe_pair();
        int resumed = 0;
        {
            io_context ioc(iocp);
            win_random_access_handle h(ioc);
            BOOST_TEST(!h.assign(test::as_native(p.server.release())));
            auto reader = [&]() -> capy::task<> {
                char buf[16];
                std::ignore = co_await h.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                ++resumed;
            };
            for (int i = 0; i < 3; ++i)
                capy::run_async(ioc.get_executor())(reader());
            for (int i = 0; i < 3; ++i)
                std::ignore = ioc.run_one();
        }
        BOOST_TEST_EQ(resumed, 0);
    }

    void run()
    {
        testDestroyWithPendingStreamHandleRead();
        testDestroyWithPendingRandomAccessHandleRead();
        testDestroyWithPendingStreamHandleWrite();
        testDestroyDrainsBothEndsOfOnePipe();
        testDestroyWithSeveralPendingRandomAccessReads();
    }
};

TEST_SUITE(iocp_handle_teardown_test, "boost.corosio.iocp_handle_teardown");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
