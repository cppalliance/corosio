//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/win_stream_handle.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/error.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/concept/read_stream.hpp>
#include <boost/capy/concept/write_stream.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>

#include <cstring>
#include <stop_token>
#include <system_error>
#include <type_traits>

#include "context.hpp"
#include "test_suite.hpp"
#include "win_test_handles.hpp"

namespace boost::corosio {

static_assert(capy::ReadStream<win_stream_handle>);
static_assert(capy::WriteStream<win_stream_handle>);
static_assert(std::is_base_of_v<io_stream, win_stream_handle>);
static_assert(!std::is_copy_constructible_v<win_stream_handle>);
static_assert(std::is_move_constructible_v<win_stream_handle>);
static_assert(std::is_same_v<
              decltype(std::declval<win_stream_handle&>().assign(
                  std::declval<native_handle_type>())),
              std::error_code>);

template<auto Backend>
struct win_stream_handle_test
{
    // Adopts p.server into h; p.server no longer owns it.
    static void adopt(win_stream_handle& h, test::pipe_pair& p)
    {
        BOOST_TEST(!h.assign(test::as_native(p.server.get())));
        p.server.release();
    }

    void testConstruction()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        BOOST_TEST(!h.is_open());
        BOOST_TEST_EQ(h.native_handle(), ~native_handle_type{});
    }

    void testAssignRejectsAnonymousPipe()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        HANDLE r = nullptr, w = nullptr;
        BOOST_TEST(::CreatePipe(&r, &w, nullptr, 0));
        test::unique_handle rr(r), ww(w);
        BOOST_TEST(
            h.assign(test::as_native(r)) == std::errc::operation_not_supported);
        BOOST_TEST(!h.is_open());
    }

    void testAssignRejectsDiskFile()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        test::temp_path t("sh_disk");
        auto f = test::open_file(t.path, true);
        BOOST_TEST(
            h.assign(test::as_native(f.get())) ==
            std::errc::operation_not_supported);
    }

    void testAssignRejectsInvalid()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        BOOST_TEST(
            h.assign(test::as_native(INVALID_HANDLE_VALUE)) ==
            std::errc::bad_file_descriptor);
    }

    void testAssignRejectsSelf()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);
        auto const held = h.native_handle();
        BOOST_TEST(h.assign(held) == error::already_open);
        BOOST_TEST_EQ(h.native_handle(), held);
    }

    void testAssignRejectsHandleBoundElsewhere()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        test::unique_handle other(
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        BOOST_TEST(::CreateIoCompletionPort(p.server.get(), other.get(), 0, 0));
        BOOST_TEST(
            h.assign(test::as_native(p.server.get())) ==
            std::errc::invalid_argument);
        BOOST_TEST(!h.is_open());
    }

    void testAssignOnOpenKeepsPendingRead()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);
        auto q = test::make_pipe_pair();

        std::error_code ec;
        std::size_t n = 0;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [rec, rn] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = rec;
            n  = rn;
        };
        auto poker = [&]() -> capy::task<> {
            BOOST_TEST(
                h.assign(test::as_native(q.server.get())) ==
                error::already_open);
            BOOST_TEST(test::write_all(p.client.get(), "x", 1));
            co_return;
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(poker());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 1u);
    }

    void testReadWrite()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        BOOST_TEST(test::write_all(p.client.get(), "ping", 4));
        std::error_code rec, wec;
        std::size_t rn = 0, wn = 0;
        char buf[8]{};
        auto task = [&]() -> capy::task<> {
            auto [e1, n1] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = e1;
            rn  = n1;
            auto [e2, n2] = co_await h.write_some(capy::const_buffer("pong", 4));
            wec = e2;
            wn  = n2;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 4u);
        BOOST_TEST(std::memcmp(buf, "ping", 4) == 0);
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 4u);

        char back[4]{};
        DWORD got = 0;
        BOOST_TEST(::ReadFile(p.client.get(), back, 4, &got, nullptr));
        BOOST_TEST(std::memcmp(back, "pong", 4) == 0);
    }

    void testConcurrentReadAndWrite()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::error_code rec, wec;
        std::size_t rn = 0, wn = 0;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = e;
            rn  = n;
        };
        auto writer = [&]() -> capy::task<> {
            auto [e, n] = co_await h.write_some(capy::const_buffer("out", 3));
            wec = e;
            wn  = n;
            // The read is still parked; feed it now.
            BOOST_TEST(test::write_all(p.client.get(), "in", 2));
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(writer());
        ioc.run();
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 2u);
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 3u);
    }

    void testEofWhenPeerCloses()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::error_code ec;
        std::size_t n = 1;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, rn] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            n  = rn;
        };
        auto closer = [&]() -> capy::task<> {
            p.client.reset();
            co_return;
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(closer());
        ioc.run();
        BOOST_TEST(ec == capy::cond::eof);
        BOOST_TEST_EQ(n, 0u);
    }

    void testWriteToClosedPeerIsBrokenPipe()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);
        p.client.reset();

        std::error_code ec;
        auto writer = [&]() -> capy::task<> {
            auto [e, n] = co_await h.write_some(capy::const_buffer("x", 1));
            ec = e;
            (void)n;
        };
        capy::run_async(ioc.get_executor())(writer());
        ioc.run();
        BOOST_TEST(ec == std::errc::broken_pipe);
    }

    void testMessageModeMoreData()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair(/*message_mode=*/true);
        adopt(h, p);
        BOOST_TEST(test::write_all(p.client.get(), "0123456789", 10));

        std::size_t sizes[3]{};
        std::error_code ecs[3];
        auto reader = [&]() -> capy::task<> {
            char buf[4];
            for (int i = 0; i < 3; ++i)
            {
                auto [e, n] =
                    co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
                ecs[i]   = e;
                sizes[i] = n;
            }
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();
        for (auto& e : ecs)
            BOOST_TEST(!e);
        BOOST_TEST_EQ(sizes[0], 4u);
        BOOST_TEST_EQ(sizes[1], 4u);
        BOOST_TEST_EQ(sizes[2], 2u);
    }

    void testZeroLength()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::error_code rec, wec;
        std::size_t rn = 1, wn = 1;
        auto task = [&]() -> capy::task<> {
            auto [e1, n1] = co_await h.read_some(capy::mutable_buffer());
            rec = e1;
            rn  = n1;
            auto [e2, n2] = co_await h.write_some(capy::const_buffer());
            wec = e2;
            wn  = n2;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 0u);
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 0u);
    }

    void testCancelPendingRead()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::error_code ec;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            (void)n;
        };
        auto canceller = [&]() -> capy::task<> {
            h.cancel();
            co_return;
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(canceller());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    void testStopTokenCancelsRead()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::stop_source ss;
        std::error_code ec;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            (void)n;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ioc.get_executor(), ss.get_token())(reader());
        capy::run_async(ioc.get_executor())(stopper());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    void testReleaseCancelsPendingRead()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);

        std::error_code ec;
        std::error_code release_ec;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            (void)n;
        };
        auto releaser = [&]() -> capy::task<> {
            // The read is in flight, so release() cancels it and throws.
            try
            {
                (void)h.release();
            }
            catch (std::system_error const& e)
            {
                release_ec = e.code();
            }
            co_return;
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(releaser());
        ioc.run();
        BOOST_TEST(release_ec == std::errc::device_or_resource_busy);
        BOOST_TEST(ec == capy::cond::canceled);
        BOOST_TEST(h.is_open());

        native_handle_type const raw = h.release();
        BOOST_TEST(!h.is_open());

        // The caller owns the released handle.
        DWORD flags = 0;
        BOOST_TEST(::GetHandleInformation(
            reinterpret_cast<HANDLE>(raw), &flags));
        ::CloseHandle(reinterpret_cast<HANDLE>(raw));
    }

    void testReleaseIdleReadopts()
    {
        auto p = test::make_pipe_pair();
        native_handle_type raw{};
        {
            io_context ioc(Backend);
            win_stream_handle h(ioc);
            adopt(h, p);
            raw = h.release();
            BOOST_TEST(!h.is_open());
        }

        io_context ioc2(Backend);
        win_stream_handle h2(ioc2);
        BOOST_TEST(!h2.assign(raw));
        BOOST_TEST(test::write_all(p.client.get(), "ok", 2));
        std::error_code ec;
        std::size_t n = 0;
        auto reader = [&]() -> capy::task<> {
            char buf[8];
            auto [e, rn] =
                co_await h2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            n  = rn;
        };
        capy::run_async(ioc2.get_executor())(reader());
        ioc2.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);
    }

    void testReleaseAfterCompletedReadReadopts()
    {
        // A finished op leaves nothing in flight, so release() detaches
        // and a second context can adopt the handle.
        auto p = test::make_pipe_pair();
        native_handle_type raw{};
        {
            io_context ioc(Backend);
            win_stream_handle h(ioc);
            adopt(h, p);
            BOOST_TEST(test::write_all(p.client.get(), "ab", 2));
            std::error_code ec;
            auto reader = [&]() -> capy::task<> {
                char buf[8];
                auto [e, n] =
                    co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
                ec = e;
                (void)n;
            };
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            BOOST_TEST(!ec);
            raw = h.release();
        }

        io_context ioc2(Backend);
        win_stream_handle h2(ioc2);
        BOOST_TEST(!h2.assign(raw));
        BOOST_TEST(test::write_all(p.client.get(), "ok", 2));
        std::error_code ec;
        std::size_t n = 0;
        auto reader = [&]() -> capy::task<> {
            char buf[8];
            auto [e, rn] =
                co_await h2.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            n  = rn;
        };
        capy::run_async(ioc2.get_executor())(reader());
        ioc2.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);
    }

    void testReleaseWhenClosedThrows()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        BOOST_TEST_THROWS(h.release(), std::system_error);
    }

    void testComposedRead()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair();
        adopt(h, p);
        BOOST_TEST(test::write_all(p.client.get(), "abc", 3));
        BOOST_TEST(test::write_all(p.client.get(), "def", 3));

        std::error_code ec;
        std::size_t n = 0;
        char buf[6]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, rn] =
                co_await capy::read(h, capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            n  = rn;
        };
        capy::run_async(ioc.get_executor())(reader());
        ioc.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 6u);
        BOOST_TEST(std::memcmp(buf, "abcdef", 6) == 0);
    }

    void testReleaseWithPendingThrowsAndKeeps()
    {
        io_context a(Backend);
        io_context b(Backend);
        win_stream_handle ha(a);
        win_stream_handle hb(b);
        auto p = test::make_pipe_pair();
        adopt(ha, p);
        native_handle_type const held = ha.native_handle();

        std::error_code rec;
        char buf[4];
        auto reader = [&]() -> capy::task<> {
            auto [e, n] =
                co_await ha.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = e;
            (void)n;
        };
        capy::run_async(a.get_executor())(reader());
        a.poll(); // the read is now issued and pending

        bool threw = false;
        try
        {
            (void)ha.release();
        }
        catch (std::system_error const& e)
        {
            threw = true;
            BOOST_TEST(e.code() == std::errc::device_or_resource_busy);
        }
        BOOST_TEST(threw);
        BOOST_TEST(ha.is_open());
        BOOST_TEST_EQ(ha.native_handle(), held);

        a.run(); // the cancelled read drains
        BOOST_TEST(rec == capy::cond::canceled);

        native_handle_type const released = ha.release(); // idle now
        BOOST_TEST_EQ(released, held);
        BOOST_TEST(!ha.is_open());
        BOOST_TEST(!hb.assign(released)); // detached: another context adopts it
    }

    void testServerDisconnectIsEof()
    {
        io_context ioc(Backend);
        win_stream_handle h(ioc);
        auto p = test::make_pipe_pair_overlapped_client();
        BOOST_TEST(!h.assign(test::as_native(p.client.get())));
        p.client.release();

        std::error_code rec, wec;
        char buf[4];
        auto t = [&]() -> capy::task<> {
            ::FlushFileBuffers(p.server.get());
            ::DisconnectNamedPipe(p.server.get());
            auto [e1, n1] =
                co_await h.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            rec = e1;
            (void)n1;
            auto [e2, n2] = co_await h.write_some(capy::const_buffer("x", 1));
            wec = e2;
            (void)n2;
        };
        capy::run_async(ioc.get_executor())(t());
        ioc.run();

        BOOST_TEST(rec == capy::cond::eof);
        BOOST_TEST(wec == std::errc::broken_pipe);
    }

    void testAssignRejectsSocket()
    {
        // A SOCKET reports FILE_TYPE_PIPE; adopting it would later close
        // it with CloseHandle.
        io_context ioc(Backend);
        tcp_socket s(ioc);
        BOOST_TEST(!s.open());
        win_stream_handle h(ioc);
        BOOST_TEST(
            h.assign(static_cast<native_handle_type>(s.native_handle())) ==
            std::errc::operation_not_supported);
        BOOST_TEST(!h.is_open());
    }

    void run()
    {
        testConstruction();
        testAssignRejectsAnonymousPipe();
        testAssignRejectsDiskFile();
        testAssignRejectsInvalid();
        testAssignRejectsSelf();
        testAssignRejectsHandleBoundElsewhere();
        testAssignOnOpenKeepsPendingRead();
        testReadWrite();
        testConcurrentReadAndWrite();
        testEofWhenPeerCloses();
        testWriteToClosedPeerIsBrokenPipe();
        testMessageModeMoreData();
        testZeroLength();
        testCancelPendingRead();
        testStopTokenCancelsRead();
        testReleaseCancelsPendingRead();
        testReleaseIdleReadopts();
        testReleaseWhenClosedThrows();
        testReleaseAfterCompletedReadReadopts();
        testComposedRead();
        testReleaseWithPendingThrowsAndKeeps();
        testServerDisconnectIsEof();
        testAssignRejectsSocket();
    }
};

COROSIO_BACKEND_TESTS(win_stream_handle_test, "boost.corosio.win_stream_handle")

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
