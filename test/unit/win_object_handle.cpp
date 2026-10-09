//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/win_object_handle.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/delay.hpp>
#include <boost/corosio/error.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/strand.hpp>
#include <boost/capy/task.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "context.hpp"
#include "test_suite.hpp"
#include "win_test_handles.hpp"

namespace boost::corosio {

static_assert(std::is_base_of_v<io_object, win_object_handle>);
static_assert(!std::is_copy_constructible_v<win_object_handle>);
static_assert(std::is_move_constructible_v<win_object_handle>);

template<auto Backend>
struct win_object_handle_test
{
    // Adopts a new event; returns the raw handle (owned by o).
    static HANDLE adopt_event(win_object_handle& o, bool manual, bool signaled)
    {
        HANDLE ev = ::CreateEventW(nullptr, manual, signaled, nullptr);
        BOOST_TEST(!o.assign(test::as_native(ev)));
        return ev;
    }

    static std::error_code wait_once(io_context& ioc, win_object_handle& o)
    {
        std::error_code ec = std::make_error_code(std::errc::io_error);
        auto task = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        capy::run_async(ioc.get_executor())(task());
        ioc.run();
        return ec;
    }

    void testConstruction()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        BOOST_TEST(!o.is_open());
        BOOST_TEST_EQ(o.native_handle(), ~native_handle_type{});
    }

    void testWaitOnClosed()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        BOOST_TEST(wait_once(ioc, o) == std::errc::bad_file_descriptor);
    }

    void testRejections()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        test::unique_handle m(::CreateMutexW(nullptr, FALSE, nullptr));
        BOOST_TEST(
            o.assign(test::as_native(m.get())) ==
            std::errc::operation_not_supported);
        BOOST_TEST(
            o.assign(test::as_native(nullptr)) ==
            std::errc::bad_file_descriptor);
        // Pseudo-handles name "whichever thread asks"; never adoptable.
        BOOST_TEST(
            o.assign(test::as_native(::GetCurrentThread())) ==
            std::errc::operation_not_supported);
        // SYNCHRONIZE alone does not make a handle a meaningful wait: a
        // file handle has it, but its signal tracks I/O, not an event.
        test::temp_path t("oh_file");
        auto f = test::open_file(t.path, false);
        BOOST_TEST(
            o.assign(test::as_native(f.get())) ==
            std::errc::operation_not_supported);
        BOOST_TEST(!o.is_open());
    }

    void testAssignRejectsSelf()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        adopt_event(o, true, false);
        BOOST_TEST(o.assign(o.native_handle()) == error::already_open);
        BOOST_TEST(o.is_open());
    }

    void testManualResetEvent()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        adopt_event(o, true, true);
        BOOST_TEST(!wait_once(ioc, o));
        ioc.restart();
        // Still signaled: a manual-reset event is not consumed.
        BOOST_TEST(!wait_once(ioc, o));
    }

    void testAutoResetEventConsumedOnce()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, true);
        BOOST_TEST(!wait_once(ioc, o));
        BOOST_TEST_EQ(::WaitForSingleObject(ev, 0), DWORD(WAIT_TIMEOUT));
    }

    void testSignalFromAnotherThread()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, false);
        std::thread t([ev] {
            ::Sleep(20);
            ::SetEvent(ev);
        });
        BOOST_TEST(!wait_once(ioc, o));
        t.join();
    }

    void testSemaphore()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE s = ::CreateSemaphoreW(nullptr, 1, 2, nullptr);
        BOOST_TEST(!o.assign(test::as_native(s)));
        BOOST_TEST(!wait_once(ioc, o));
        BOOST_TEST_EQ(::WaitForSingleObject(s, 0), DWORD(WAIT_TIMEOUT));
    }

    void testChildProcessExit()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        wchar_t cmd[] = L"cmd.exe /c exit 3";
        BOOST_TEST(::CreateProcessW(
            nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            nullptr, &si, &pi));
        ::CloseHandle(pi.hThread);
        BOOST_TEST(!o.assign(test::as_native(pi.hProcess)));
        BOOST_TEST(!wait_once(ioc, o));
        DWORD code = 0;
        BOOST_TEST(::GetExitCodeProcess(pi.hProcess, &code));
        BOOST_TEST_EQ(code, 3u);
    }

    void testWaitableTimer()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE t = ::CreateWaitableTimerW(nullptr, TRUE, nullptr);
        LARGE_INTEGER due;
        due.QuadPart = -500000; // 50 ms, relative
        BOOST_TEST(::SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE));
        BOOST_TEST(!o.assign(test::as_native(t)));
        BOOST_TEST(!wait_once(ioc, o));
    }

    void testSecondWaitIsInProgress()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, false);
        std::error_code first, second;
        auto w1 = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            first = e;
        };
        auto w2 = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            second = e;
            ::SetEvent(ev);
        };
        capy::run_async(ioc.get_executor())(w1());
        capy::run_async(ioc.get_executor())(w2());
        ioc.run();
        BOOST_TEST(second == std::errc::operation_in_progress);
        BOOST_TEST(!first);
    }

    void testCancel()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, false);
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        auto canceller = [&]() -> capy::task<> {
            o.cancel();
            co_return;
        };
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(canceller());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
        // A cancelled wait consumed nothing; the object still works.
        ::SetEvent(ev);
        ioc.restart();
        BOOST_TEST(!wait_once(ioc, o));
    }

    void testStopToken()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        adopt_event(o, false, false);
        std::stop_source ss;
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        auto stopper = [&]() -> capy::task<> {
            ss.request_stop();
            co_return;
        };
        capy::run_async(ioc.get_executor(), ss.get_token())(waiter());
        capy::run_async(ioc.get_executor())(stopper());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
    }

    // The stop lands before the wait is published, so wait() itself
    // must claim the op for it, consuming no signal.
    void testStopTokenStoppedBeforeWait()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, true);
        std::stop_source ss;
        ss.request_stop();
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        capy::run_async(ioc.get_executor(), ss.get_token())(waiter());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
        BOOST_TEST(::WaitForSingleObject(ev, 0) == WAIT_OBJECT_0);
    }

    // Busy-waits about @p n iterations; volatile keeps the loop.
    static void spin(int n) noexcept
    {
        volatile int sink = 0;
        for (int k = 0; k < n; ++k)
            sink = k;
        (void)sink;
    }

    // SetEvent satisfies the pending wait before it returns: the kernel
    // consumes the auto-reset signal and queues the pool callback. A
    // cancel() issued afterwards must therefore drain that callback and
    // report success, never cancel it. Deterministic regardless of how
    // many CPUs the run gets, so this pins the signal-wins side that the
    // timing race below cannot guarantee to reach on a starved runner.
    void testSignalBeforeCancelReportsSuccess()
    {
        int lost = 0;
        for (int i = 0; i < 200; ++i)
        {
            io_context ioc(Backend);
            win_object_handle o(ioc);
            HANDLE ev = adopt_event(o, false, false);
            std::error_code ec = std::make_error_code(std::errc::io_error);
            auto waiter = [&]() -> capy::task<> {
                auto [e] = co_await o.wait();
                ec = e;
            };
            auto signaler = [&]() -> capy::task<> {
                ::SetEvent(ev);
                o.cancel();
                co_return;
            };
            capy::run_async(ioc.get_executor())(waiter());
            capy::run_async(ioc.get_executor())(signaler());
            ioc.run();
            if (ec || ::WaitForSingleObject(ev, 0) != WAIT_TIMEOUT)
                ++lost;
        }
        BOOST_TEST_EQ(lost, 0);
    }

    // A signal racing cancel() must be reported by exactly one party:
    // the wait reports success, or the event is still signaled.
    //
    // Both threads start from a shared spin flag, then the racer delays
    // cancel() by an amount that tracks the race boundary: longer after
    // cancel wins, shorter after the signal wins. The boundary (thread
    // wake-up latency) differs by machine and compiler. A fixed delay
    // that split the outcomes on MSVC let cancel win every time on
    // MinGW, and spawning the thread then calling cancel() at once lets
    // cancel win every time. Signaling inline always lets the signal
    // win, because the pool marks the wait satisfied inside SetEvent.
    //
    // Which side wins is not asserted. When the setter thread gets no CPU
    // while the racer spins (one core, or a loaded CI runner) cancel wins
    // every iteration. The two outcomes are pinned deterministically by
    // testCancel and testSignalBeforeCancelReportsSuccess; this loop only
    // checks that no interleaving it reaches loses or invents a signal.
    void testSignalRacingCancelIsNeverLost()
    {
        int violations = 0;
        int delay      = 0;
        for (int i = 0; i < 2000; ++i)
        {
            io_context ioc(Backend);
            win_object_handle o(ioc);
            HANDLE ev = adopt_event(o, false, false);
            std::error_code ec = std::make_error_code(std::errc::io_error);
            auto waiter = [&]() -> capy::task<> {
                auto [e] = co_await o.wait();
                ec = e;
            };
            auto racer = [&]() -> capy::task<> {
                std::atomic<bool> go{false};
                std::thread t([ev, &go, i] {
                    while (!go.load(std::memory_order_acquire))
                    {
                    }
                    spin(i % 64);
                    ::SetEvent(ev);
                });
                go.store(true, std::memory_order_release);
                spin(delay);
                o.cancel();
                t.join();
                co_return;
            };
            capy::run_async(ioc.get_executor())(waiter());
            capy::run_async(ioc.get_executor())(racer());
            ioc.run();
            bool const signaled = ::WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
            bool const reported = !ec;
            if (reported == signaled)
                ++violations;
            if (reported)
                delay -= delay / 4;
            else
                // Capped so a run the signal never wins stays short.
                delay = (std::min)(delay + delay / 4 + 64, 1 << 20);
            BOOST_TEST(reported || ec == capy::cond::canceled);
        }
        BOOST_TEST_EQ(violations, 0);
    }

    void testCloseWithPendingWait()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        adopt_event(o, false, false);
        std::error_code ec;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        auto closer = [&]() -> capy::task<> {
            o.close();
            co_return;
        };
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(closer());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
        BOOST_TEST(!o.is_open());
    }

    void testReleaseWithPendingWait()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, false, false);
        std::error_code ec;
        native_handle_type raw{};
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec = e;
        };
        auto releaser = [&]() -> capy::task<> {
            raw = o.release();
            co_return;
        };
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(releaser());
        ioc.run();
        BOOST_TEST(ec == capy::cond::canceled);
        BOOST_TEST_EQ(raw, test::as_native(ev));
        // Still a live handle owned by the caller.
        BOOST_TEST(::SetEvent(ev));
        ::CloseHandle(ev);
    }

    void testDestroyWithPendingWait()
    {
        bool resumed = false;
        {
            io_context ioc(Backend);
            auto waiter = [&]() -> capy::task<> {
                win_object_handle o(ioc);
                adopt_event(o, false, false);
                std::ignore = co_await o.wait();
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(waiter());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testMoveThenWait()
    {
        io_context ioc(Backend);
        win_object_handle a(ioc);
        adopt_event(a, true, true);
        BOOST_TEST(!wait_once(ioc, a));
        win_object_handle b(std::move(a));
        ioc.restart();
        BOOST_TEST(!wait_once(ioc, b));
    }

    void testStrandWaitersNeverShareAContinuation()
    {
        // A manual-reset event stays signaled, so every wait completes
        // at once. Waiters on a strand resume through a posted
        // continuation; a second waiter must never reuse one that is
        // still queued. Both waiters share the strand, so wait() is
        // never called concurrently, which the object does not allow.
        io_context ioc(Backend, 2);
        capy::strand s(ioc.get_executor());
        win_object_handle o(ioc);
        adopt_event(o, /*manual=*/true, /*signaled=*/true);

        constexpr int per_waiter = 200;
        std::atomic<int> ok{0}, busy{0}, other{0};
        auto waiter = [&]() -> capy::task<> {
            for (int i = 0; i < per_waiter; ++i)
            {
                auto [e] = co_await o.wait();
                if (!e)
                    ++ok;
                else if (e == std::errc::operation_in_progress)
                    ++busy;
                else
                    ++other;
            }
        };
        capy::run_async(s)(waiter());
        capy::run_async(s)(waiter());
        std::thread t([&] { ioc.run(); });
        ioc.run();
        t.join();

        BOOST_TEST_EQ(ok + busy, 2 * per_waiter);
        BOOST_TEST_EQ(other.load(), 0);
    }

    void testLocklessContextRefusesObjectHandle()
    {
        // The pool callback completes from a foreign thread, which a
        // lockless scheduler cannot accept.
        io_context_options opts;
        opts.locking = locking_mode::unsafe;
        io_context ioc(Backend, opts);
        win_object_handle o(ioc);
        test::unique_handle ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        BOOST_TEST(
            o.assign(test::as_native(ev.get())) ==
            std::errc::operation_not_supported);
        BOOST_TEST(!o.is_open());
    }

    // Completion wins against every rundown, not just cancel(): the
    // signal is set inline, so the pool satisfies the wait first.
    template<class Rundown>
    static int signal_then_rundown_losses(Rundown rundown)
    {
        int lost = 0;
        for (int i = 0; i < 200; ++i)
        {
            io_context ioc(Backend);
            std::optional<win_object_handle> o(std::in_place, ioc);
            HANDLE ev          = adopt_event(*o, false, false);
            std::error_code ec = std::make_error_code(std::errc::io_error);
            native_handle_type released = ~native_handle_type{};
            auto waiter = [&]() -> capy::task<> {
                auto [e] = co_await o->wait();
                ec       = e;
            };
            auto signaler = [&]() -> capy::task<> {
                ::SetEvent(ev);
                rundown(o, released);
                co_return;
            };
            capy::run_async(ioc.get_executor())(waiter());
            capy::run_async(ioc.get_executor())(signaler());
            ioc.run();
            if (ec)
                ++lost;
            if (released != ~native_handle_type{})
                ::CloseHandle(reinterpret_cast<HANDLE>(released));
        }
        return lost;
    }

    void testSignalBeforeCloseReportsSuccess()
    {
        BOOST_TEST_EQ(
            signal_then_rundown_losses(
                [](auto& o, native_handle_type&) { o->close(); }),
            0);
    }

    void testSignalBeforeReleaseReportsSuccess()
    {
        BOOST_TEST_EQ(
            signal_then_rundown_losses(
                [](auto& o, native_handle_type& r) { r = o->release(); }),
            0);
    }

    void testSignalBeforeDestroyReportsSuccess()
    {
        BOOST_TEST_EQ(
            signal_then_rundown_losses(
                [](auto& o, native_handle_type&) { o.reset(); }),
            0);
    }

    void testAssignOnOpenKeepsPendingWait()
    {
        io_context ioc(Backend);
        win_object_handle o(ioc);
        HANDLE ev = adopt_event(o, /*manual=*/true, /*signaled=*/false);
        test::unique_handle other(::CreateEventW(nullptr, TRUE, TRUE, nullptr));

        std::error_code ec;
        bool done = false;
        auto waiter = [&]() -> capy::task<> {
            auto [e] = co_await o.wait();
            ec       = e;
            done     = true;
        };
        auto poker = [&]() -> capy::task<> {
            auto [d] = co_await delay(std::chrono::milliseconds(10));
            (void)d;
            BOOST_TEST(
                o.assign(test::as_native(other.get())) == error::already_open);
            BOOST_TEST_EQ(done, false); // the pending wait is undisturbed
            ::SetEvent(ev);
        };
        capy::run_async(ioc.get_executor())(waiter());
        capy::run_async(ioc.get_executor())(poker());
        ioc.run();

        BOOST_TEST(done);
        BOOST_TEST(!ec);
    }

    void run()
    {
        testConstruction();
        testWaitOnClosed();
        testRejections();
        testAssignRejectsSelf();
        testManualResetEvent();
        testAutoResetEventConsumedOnce();
        testSignalFromAnotherThread();
        testSemaphore();
        testChildProcessExit();
        testWaitableTimer();
        testSecondWaitIsInProgress();
        testCancel();
        testStopToken();
        testStopTokenStoppedBeforeWait();
        testSignalBeforeCancelReportsSuccess();
        testSignalRacingCancelIsNeverLost();
        testCloseWithPendingWait();
        testReleaseWithPendingWait();
        testDestroyWithPendingWait();
        testMoveThenWait();
        testStrandWaitersNeverShareAContinuation();
        testLocklessContextRefusesObjectHandle();
        testSignalBeforeCloseReportsSuccess();
        testSignalBeforeReleaseReportsSuccess();
        testSignalBeforeDestroyReportsSuccess();
        testAssignOnOpenKeepsPendingWait();
    }
};

COROSIO_BACKEND_TESTS(win_object_handle_test, "boost.corosio.win_object_handle")

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
