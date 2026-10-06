//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/win_random_access_handle.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/error.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/capy/task.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stop_token>
#include <system_error>
#include <type_traits>

#include "context.hpp"
#include "test_suite.hpp"
#include "win_test_handles.hpp"

namespace boost::corosio {

static_assert(std::is_base_of_v<io_object, win_random_access_handle>);
static_assert(!std::is_copy_constructible_v<win_random_access_handle>);
static_assert(std::is_move_constructible_v<win_random_access_handle>);

template<auto Backend>
struct win_random_access_handle_test
{
    struct fixture
    {
        test::temp_path path{"rah"};
        io_context ioc{Backend};
        win_random_access_handle h{ioc};

        fixture()
        {
            auto f = test::open_file(path.path, true);
            BOOST_TEST(!h.assign(test::as_native(f.release())));
        }
    };

    void testConstruction()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        BOOST_TEST(!h.is_open());
        BOOST_TEST_EQ(h.native_handle(), ~native_handle_type{});
    }

    void testAcceptsOverlappedPipe()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!h.assign(test::as_native(p.server.release())));
    }

    void testRejections()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        test::temp_path t("rah_sync");
        auto sync = test::open_file(t.path, false);
        BOOST_TEST(
            h.assign(test::as_native(sync.get())) ==
            std::errc::operation_not_supported);
        auto dir = test::open_directory(std::filesystem::temp_directory_path());
        BOOST_TEST(
            h.assign(test::as_native(dir.get())) ==
            std::errc::operation_not_supported);
        BOOST_TEST(
            h.assign(test::as_native(nullptr)) ==
            std::errc::bad_file_descriptor);
        BOOST_TEST(!h.is_open());
    }

    void testPositionalWriteThenRead()
    {
        fixture fx;
        std::error_code wec, rec;
        std::size_t wn = 0, rn = 0;
        char buf[4]{};
        auto task = [&]() -> capy::task<> {
            auto [e1, n1] =
                co_await fx.h.write_some_at(100, capy::const_buffer("wxyz", 4));
            wec = e1;
            wn  = n1;
            auto [e2, n2] =
                co_await fx.h.read_some_at(102, capy::mutable_buffer(buf, 2));
            rec = e2;
            rn  = n2;
        };
        capy::run_async(fx.ioc.get_executor())(task());
        fx.ioc.run();
        BOOST_TEST(!wec);
        BOOST_TEST_EQ(wn, 4u);
        BOOST_TEST(!rec);
        BOOST_TEST_EQ(rn, 2u);
        BOOST_TEST(std::memcmp(buf, "yz", 2) == 0);
    }

    void testManyConcurrentWrites()
    {
        fixture fx;
        constexpr int count = 32;
        int ok = 0;
        auto writer = [&](int i) -> capy::task<> {
            char c = static_cast<char>('A' + (i % 26));
            auto [e, n] = co_await fx.h.write_some_at(
                static_cast<std::uint64_t>(i), capy::const_buffer(&c, 1));
            if (!e && n == 1)
                ++ok;
        };
        for (int i = 0; i < count; ++i)
            capy::run_async(fx.ioc.get_executor())(writer(i));
        fx.ioc.run();
        BOOST_TEST_EQ(ok, count);

        char buf[count]{};
        std::size_t n = 0;
        auto reader = [&]() -> capy::task<> {
            auto [e, rn] =
                co_await fx.h.read_some_at(0, capy::mutable_buffer(buf, count));
            (void)e;
            n = rn;
        };
        capy::run_async(fx.ioc.get_executor())(reader());
        fx.ioc.restart();
        fx.ioc.run();
        BOOST_TEST_EQ(n, std::size_t(count));
        for (int i = 0; i < count; ++i)
            BOOST_TEST_EQ(buf[i], static_cast<char>('A' + (i % 26)));
    }

    void testReadPastEndIsEof()
    {
        fixture fx;
        std::error_code ec;
        auto task = [&]() -> capy::task<> {
            char buf[4];
            auto [e, n] =
                co_await fx.h.read_some_at(4096, capy::mutable_buffer(buf, 4));
            ec = e;
            (void)n;
        };
        capy::run_async(fx.ioc.get_executor())(task());
        fx.ioc.run();
        BOOST_TEST(ec == capy::cond::eof);
    }

    void testCancelPendingPipeReads()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!h.assign(test::as_native(p.server.release())));
        int canceled = 0;
        auto reader = [&]() -> capy::task<> {
            char buf[4];
            auto [e, n] = co_await h.read_some_at(0, capy::mutable_buffer(buf, 4));
            (void)n;
            if (e == capy::cond::canceled)
                ++canceled;
        };
        auto canceller = [&]() -> capy::task<> {
            h.cancel();
            co_return;
        };
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(canceller());
        ioc.run();
        BOOST_TEST_EQ(canceled, 2);
    }

    void testFailedAssignKeepsHeld()
    {
        fixture fx;
        auto const held = fx.h.native_handle();
        HANDLE r = nullptr, w = nullptr;
        BOOST_TEST(::CreatePipe(&r, &w, nullptr, 0));
        test::unique_handle rr(r), ww(w);
        BOOST_TEST(fx.h.assign(test::as_native(r)) == error::already_open);
        BOOST_TEST_EQ(fx.h.native_handle(), held);
    }

    void testReleaseAndReadopt()
    {
        native_handle_type raw{};
        test::temp_path t("rah_readopt");
        {
            io_context ioc(Backend);
            win_random_access_handle h(ioc);
            auto f = test::open_file(t.path, true);
            BOOST_TEST(!h.assign(test::as_native(f.release())));
            raw = h.release();
            BOOST_TEST(!h.is_open());
        }
        io_context ioc2(Backend);
        win_random_access_handle h2(ioc2);
        BOOST_TEST(!h2.assign(raw));
    }

    void testReleaseAfterCompletedReadReadopts()
    {
        // A finished op leaves nothing in flight, so release() detaches
        // and a second context can adopt the handle.
        test::temp_path t("rah_done_readopt");
        {
            auto w = test::open_file(t.path, false);
            BOOST_TEST(test::write_all(w.get(), "abcd", 4));
        }
        native_handle_type raw{};
        {
            io_context ioc(Backend);
            win_random_access_handle h(ioc);
            auto f = test::open_file(t.path, true);
            BOOST_TEST(!h.assign(test::as_native(f.release())));
            std::error_code ec;
            auto reader = [&]() -> capy::task<> {
                char buf[4];
                auto [e, n] = co_await h.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                ec = e;
                (void)n;
            };
            capy::run_async(ioc.get_executor())(reader());
            ioc.run();
            BOOST_TEST(!ec);
            raw = h.release();
        }

        io_context ioc2(Backend);
        win_random_access_handle h2(ioc2);
        BOOST_TEST(!h2.assign(raw));
        std::error_code ec;
        std::size_t n = 0;
        char buf[4]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, rn] = co_await h2.read_some_at(
                1, capy::mutable_buffer(buf, 2));
            ec = e;
            n  = rn;
        };
        capy::run_async(ioc2.get_executor())(reader());
        ioc2.run();
        BOOST_TEST(!ec);
        BOOST_TEST_EQ(n, 2u);
        BOOST_TEST(std::memcmp(buf, "bc", 2) == 0);
    }

    void testResumesOnAwaitingExecutor()
    {
        io_context ioc(Backend);
        capy::strand s(ioc.get_executor());
        win_random_access_handle h(ioc);
        test::temp_path p("ra_strand");
        {
            // write_all needs a synchronous handle.
            auto w = test::open_file(p.path, /*overlapped=*/false);
            BOOST_TEST(test::write_all(w.get(), "abcdefgh", 8));
        }
        auto f = test::open_file(p.path, /*overlapped=*/true);
        BOOST_TEST(!h.assign(test::as_native(f.get())));
        f.release();

        bool on_strand = false;
        std::error_code ec;
        char buf[4];
        auto t = [&]() -> capy::task<> {
            auto [e, n] = co_await h.read_some_at(
                2, capy::mutable_buffer(buf, sizeof(buf)));
            ec = e;
            (void)n;
            on_strand = s.running_in_this_thread();
        };
        capy::run_async(s)(t());
        ioc.run();

        BOOST_TEST(!ec);
        BOOST_TEST(on_strand);
    }

    void testReleaseWithPendingThrowsAndKeeps()
    {
        io_context a(Backend);
        io_context b(Backend);
        win_random_access_handle ha(a);
        win_random_access_handle hb(b);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!ha.assign(test::as_native(p.server.release())));
        native_handle_type const held = ha.native_handle();

        std::error_code rec;
        char buf[4];
        auto reader = [&]() -> capy::task<> {
            auto [e, n] = co_await ha.read_some_at(
                0, capy::mutable_buffer(buf, sizeof(buf)));
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

    void testAssignRejectsInvalid()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        BOOST_TEST(
            h.assign(test::as_native(INVALID_HANDLE_VALUE)) ==
            std::errc::bad_file_descriptor);
    }

    void testAssignRejectsSelf()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!h.assign(test::as_native(p.server.release())));
        auto const held = h.native_handle();
        BOOST_TEST(h.assign(held) == error::already_open);
        BOOST_TEST_EQ(h.native_handle(), held);
    }

    void testStopTokenCancelsRead()
    {
        io_context ioc(Backend);
        win_random_access_handle h(ioc);
        auto p = test::make_pipe_pair();
        BOOST_TEST(!h.assign(test::as_native(p.server.release())));

        std::stop_source ss;
        std::error_code ec;
        char buf[8]{};
        auto reader = [&]() -> capy::task<> {
            auto [e, n] = co_await h.read_some_at(
                0, capy::mutable_buffer(buf, sizeof(buf)));
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

    void run()
    {
        testConstruction();
        testAcceptsOverlappedPipe();
        testRejections();
        testPositionalWriteThenRead();
        testManyConcurrentWrites();
        testReadPastEndIsEof();
        testCancelPendingPipeReads();
        testFailedAssignKeepsHeld();
        testReleaseAndReadopt();
        testResumesOnAwaitingExecutor();
        testReleaseAfterCompletedReadReadopts();
        testReleaseWithPendingThrowsAndKeeps();
        testAssignRejectsInvalid();
        testAssignRejectsSelf();
        testStopTokenCancelsRead();
    }
};

COROSIO_BACKEND_TESTS(
    win_random_access_handle_test, "boost.corosio.win_random_access_handle")

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
