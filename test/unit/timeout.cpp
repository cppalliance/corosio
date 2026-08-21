//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/timeout.hpp>

#include <boost/corosio/delay.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>
#include <boost/capy/io/any_read_stream.hpp>
#include <boost/capy/io/any_write_stream.hpp>
#include <boost/capy/io_result.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/test/read_stream.hpp>
#include <boost/capy/test/write_stream.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <ratio>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <thread>
#include <vector>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Immediately-resuming awaitable yielding io_result<int>, used to
// verify payload preservation through timeout()'s pass-through path.
struct payload_awaitable
{
    int value_;

    explicit payload_awaitable(int value) : value_(value) {}

    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> h, capy::io_env const*)
    {
        return h;
    }
    capy::io_result<int> await_resume() noexcept
    {
        return {std::error_code(), value_};
    }
};

// Only completes when cancelled, via delay()'s own (well-tested)
// stop-token handling, returning a non-default payload alongside
// cond::canceled. Used to confirm that timeout()'s deadline-win path
// discards the inner payload rather than merely rewriting `ec` in
// place.
inline capy::task<capy::io_result<int>>
stop_only_payload()
{
    auto [ec] = co_await delay(std::chrono::seconds(10));
    co_return capy::io_result<int>{ec, 99};
}

template<auto Backend>
struct timeout_test
{
    void testInnerWins()
    {
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& ok_out) -> capy::task<> {
            // Inner (5ms) completes before the 10s deadline
            auto [ec] = co_await timeout(
                delay(std::chrono::milliseconds(5)),
                std::chrono::seconds(10));
            ok_out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testDeadlineWins()
    {
        io_context ioc(Backend);
        bool timed_out = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::milliseconds(5));
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out));

        ioc.run();
        BOOST_TEST(timed_out);
    }

    void testTimePointDeadlineWins()
    {
        io_context ioc(Backend);
        bool timed_out = false;

        auto t = [](bool& out) -> capy::task<> {
            auto tp = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(5);
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)), tp);
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out));

        ioc.run();
        BOOST_TEST(timed_out);
    }

    void testParentCancelReturnsCanceled()
    {
        io_context ioc(Backend);
        std::stop_source src;
        bool canceled = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::seconds(10));
            out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run_one();
        src.request_stop();
        ioc.run();
        BOOST_TEST(canceled);
    }

    void testInnerErrorPassesThrough()
    {
        // An inner op that completes with its own (non-canceled)
        // error keeps that error even though a deadline is armed.
        io_context ioc(Backend);
        bool got_error = false;

        struct erroring_awaitable
        {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> h, capy::io_env const*)
            {
                return h;
            }
            capy::io_result<> await_resume() noexcept
            {
                return {make_error_code(
                    std::errc::connection_refused)};
            }
        };

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                erroring_awaitable{}, std::chrono::seconds(10));
            out = (ec == std::errc::connection_refused);
        };
        capy::run_async(ioc.get_executor())(t(got_error));

        ioc.run();
        BOOST_TEST(got_error);
    }

    void testInnerExceptionPropagates()
    {
        io_context ioc(Backend);
        bool caught = false;

        struct throwing_awaitable
        {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> h, capy::io_env const*)
            {
                return h;
            }
            capy::io_result<> await_resume()
            {
                throw std::runtime_error("boom");
            }
        };

        auto t = [](bool& out) -> capy::task<> {
            try
            {
                std::ignore = co_await timeout(
                    throwing_awaitable{}, std::chrono::seconds(10));
            }
            catch(std::runtime_error const&)
            {
                out = true;
            }
        };
        capy::run_async(ioc.get_executor())(t(caught));

        ioc.run();
        BOOST_TEST(caught);
    }

    void testSingleThreadedHint()
    {
        io_context ioc(Backend, 1u);
        bool timed_out = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::milliseconds(5));
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out));

        ioc.run();
        BOOST_TEST(timed_out);
    }

    void testPayloadPreservedOnInnerWin()
    {
        io_context ioc(Backend);
        bool ok = false;
        int value = 0;

        auto t = [](bool& ok_out, int& value_out) -> capy::task<> {
            auto [ec, n] = co_await timeout(
                payload_awaitable(42), std::chrono::seconds(10));
            ok_out = !ec;
            value_out = n;
        };
        capy::run_async(ioc.get_executor())(t(ok, value));

        ioc.run();
        BOOST_TEST(ok);
        BOOST_TEST_EQ(value, 42);
    }

    void testReadyInnerCompletesWithoutSuspend()
    {
        // An inner op that is already ready at co_await must complete
        // through timeout() on the non-suspending path: await_suspend
        // is never driven and no deadline timer is armed.
        io_context ioc(Backend);
        bool ok = false;
        bool suspended = false;

        struct ready_awaitable
        {
            int value_;
            bool* suspended_;

            bool await_ready() const noexcept { return true; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> h, capy::io_env const*)
            {
                *suspended_ = true;
                return h;
            }
            capy::io_result<int> await_resume() noexcept
            {
                return {std::error_code(), value_};
            }
        };

        auto t = [](bool& out, bool& susp) -> capy::task<> {
            auto [ec, v] = co_await timeout(
                ready_awaitable{42, &susp}, std::chrono::milliseconds(1));
            out = !ec && v == 42;
        };
        capy::run_async(ioc.get_executor())(t(ok, suspended));

        ioc.run();
        BOOST_TEST(ok);
        BOOST_TEST(!suspended);
    }

    void testReadyInnerCanceledNotRemapped()
    {
        // A genuine cancellation surfacing from an already-ready inner
        // op must pass through unchanged: the deadline never fired, so
        // the canceled-to-timeout remap must not trigger on the
        // non-suspending path's untouched stop state.
        io_context ioc(Backend);
        bool canceled = false;

        struct ready_canceled_awaitable
        {
            bool await_ready() const noexcept { return true; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<> h, capy::io_env const*)
            {
                return h;
            }
            capy::io_result<> await_resume() noexcept
            {
                return {make_error_code(capy::error::canceled)};
            }
        };

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                ready_canceled_awaitable{}, std::chrono::seconds(10));
            out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    void testTimeoutResultHasDefaultPayload()
    {
        // The inner op only completes when cancelled (returning a
        // non-default payload of 99 alongside cond::canceled). When
        // the deadline wins, timeout() must produce a fresh result
        // with a default-initialized payload, not the inner's 99.
        io_context ioc(Backend);
        bool timed_out = false;
        int value = -1;

        auto t = [](bool& out, int& value_out) -> capy::task<> {
            auto [ec, n] = co_await timeout(
                stop_only_payload(),
                std::chrono::milliseconds(5));
            out = (ec == capy::cond::timeout);
            value_out = n;
        };
        capy::run_async(ioc.get_executor())(t(timed_out, value));

        ioc.run();
        BOOST_TEST(timed_out);
        BOOST_TEST_EQ(value, 0);
    }

    void testNestedTimeouts()
    {
        // An outer timeout wraps an inner timeout(). The inner
        // deadline (5ms) is shorter than the outer (10s), so the
        // inner timeout wins the race and its result (ec ==
        // cond::timeout) passes through the outer unchanged.
        io_context ioc(Backend);
        bool timed_out = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                timeout(
                    delay(std::chrono::seconds(10)),
                    std::chrono::milliseconds(5)),
                std::chrono::seconds(10));
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out));

        ioc.run();
        BOOST_TEST(timed_out);
    }

    void testTimeoutAlreadyElapsedDeadline()
    {
        // A deadline already in the past, or a zero duration, still
        // races the long inner op and resolves as cond::timeout.
        {
            io_context ioc(Backend);
            bool timed_out = false;

            auto t = [](bool& out) -> capy::task<> {
                auto tp = std::chrono::steady_clock::now() -
                    std::chrono::seconds(1);
                auto [ec] = co_await timeout(
                    delay(std::chrono::seconds(10)), tp);
                out = (ec == capy::cond::timeout);
            };
            capy::run_async(ioc.get_executor())(t(timed_out));

            ioc.run();
            BOOST_TEST(timed_out);
        }
        {
            io_context ioc(Backend);
            bool timed_out = false;

            auto t = [](bool& out) -> capy::task<> {
                auto [ec] = co_await timeout(
                    delay(std::chrono::seconds(10)),
                    std::chrono::milliseconds(0));
                out = (ec == capy::cond::timeout);
            };
            capy::run_async(ioc.get_executor())(t(timed_out));

            ioc.run();
            BOOST_TEST(timed_out);
        }
    }

    void testShutdownWithSuspendedTimeout()
    {
        // Destroying the io_context while a timeout() is suspended must
        // drain both the inner op's waiter and the parked timeout_coro
        // from the heap, and destroy the awaiting frame cleanly.
        int destroyed = 0;

        {
            io_context ioc(Backend);

            auto task = [](int& counter) -> capy::task<> {
                struct guard
                {
                    int& c_;
                    ~guard() { ++c_; }
                };
                guard g{counter};
                [[maybe_unused]] auto [ec] = co_await timeout(
                    delay(std::chrono::hours(1)),
                    std::chrono::hours(1));
            };

            capy::run_async(ioc.get_executor())(task(destroyed));
            ioc.poll();
            // io_context destructs with the timeout still suspended.
        }

        BOOST_TEST_EQ(destroyed, 1);
    }

    void testTimeoutOverSocket()
    {
        // A read on a socket whose peer never sends must resolve as
        // cond::timeout once the deadline elapses (mirrors the doc
        // example).
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        // s2 is held open but silent.
        [[maybe_unused]] auto [s1, s2] = test::make_socket_pair(ioc);
        bool timed_out = false;
        std::array<char, 32> buf{};

        auto t = [&]() -> capy::task<> {
            [[maybe_unused]] auto [ec, n] = co_await timeout(
                s1.read_some(capy::mutable_buffer(buf.data(), buf.size())),
                std::chrono::milliseconds(50));
            timed_out = (ec == capy::cond::timeout);
        };
        capy::run_async(ex)(t());
        ioc.run();

        BOOST_TEST(timed_out);
    }

    void testTypeErasedInnerFullProtocol()
    {
        // Type-erased stream wrappers construct their cached inner
        // awaitable inside await_ready; timeout() must drive the full
        // awaiter protocol or the vtable dispatches into raw storage
        // and the read yields garbage instead of the provided bytes.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& out) -> capy::task<> {
            capy::test::read_stream mock;
            mock.provide("hello world");
            capy::any_read_stream stream(std::move(mock));

            std::array<char, 64> buf{};
            auto [ec, n] = co_await timeout(
                stream.read_some(
                    capy::mutable_buffer(buf.data(), buf.size())),
                std::chrono::seconds(10));
            out = !ec && n == 11 &&
                std::string_view(buf.data(), n) == "hello world";
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testTypeErasedDeadlineWins()
    {
        // The deadline must cancel a genuinely pending read through
        // the type-erased vtable and resolve as cond::timeout, with
        // the wrapper tearing down its cached awaitable on the cancel
        // path. The pointer form exercises non-owning reference mode.
        io_context ioc(Backend);
        auto ex = ioc.get_executor();
        // s2 is held open but silent.
        [[maybe_unused]] auto [s1, s2] = test::make_socket_pair(ioc);
        bool timed_out = false;
        std::array<char, 32> buf{};

        auto t = [&]() -> capy::task<> {
            capy::any_read_stream stream(&s1);
            [[maybe_unused]] auto [ec, n] = co_await timeout(
                stream.read_some(
                    capy::mutable_buffer(buf.data(), buf.size())),
                std::chrono::milliseconds(50));
            timed_out = (ec == capy::cond::timeout);
        };
        capy::run_async(ex)(t());
        ioc.run();

        BOOST_TEST(timed_out);
    }

    void testNestedTimeoutOverTypeErased()
    {
        // Awaiter-protocol forwarding must be transitive: stacked
        // timeout adapters have to relay await_ready down to the
        // type-erased wrapper so its setup still runs.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& out) -> capy::task<> {
            capy::test::read_stream mock;
            mock.provide("hi");
            capy::any_read_stream stream(std::move(mock));

            std::array<char, 8> buf{};
            auto [ec, n] = co_await timeout(
                timeout(
                    stream.read_some(
                        capy::mutable_buffer(buf.data(), buf.size())),
                    std::chrono::seconds(10)),
                std::chrono::seconds(10));
            out = !ec && n == 2 &&
                std::string_view(buf.data(), n) == "hi";
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testTypeErasedWriteFullProtocol()
    {
        // The write-side wrapper shares the construct-in-await_ready
        // pattern; a timeout-wrapped write must deliver real bytes
        // into the underlying stream, not dispatch into raw storage.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& out) -> capy::task<> {
            std::string_view const msg = "hello world";
            capy::test::write_stream mock;
            capy::any_write_stream stream(&mock);

            auto [ec, n] = co_await timeout(
                stream.write_some(
                    capy::const_buffer(msg.data(), msg.size())),
                std::chrono::seconds(10));
            out = !ec && n == msg.size() && mock.data() == msg;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testAlreadyStoppedCompletesCanceled()
    {
        // Stop already requested at suspension: the inner op cancels and
        // timeout() reports cond::canceled, never cond::timeout.
        io_context ioc(Backend);
        std::stop_source src;
        src.request_stop();
        bool canceled = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::seconds(10));
            out = (ec == capy::cond::canceled);
        };
        capy::run_async(ioc.get_executor(), src.get_token())(t(canceled));

        ioc.run();
        BOOST_TEST(canceled);
    }

    // A non-io_context executor cannot supply a timer service; calling
    // await_suspend directly (outside the noexcept resumption path)
    // exercises the precondition diagnostic catchably.
    void testNonIoContextThrows()
    {
        capy::thread_pool pool(1);
        auto ex = pool.get_executor();
        auto ta = timeout(
            payload_awaitable(0), std::chrono::milliseconds(1));
        capy::io_env env{ex, {}, {}};
        BOOST_TEST_THROWS(
            ta.await_suspend(std::noop_coroutine(), &env),
            std::logic_error);
    }

    void testNarrowRepDeadlineClamp()
    {
        // A narrow-rep deadline must survive the overflow clamp: the old
        // clamp converted nanoseconds::max() into the caller's rep,
        // wrapping a 5ms deadline out to ~292 years so it would never
        // fire. The deadline must win promptly instead.
        io_context ioc(Backend);
        bool timed_out = false;
        std::chrono::steady_clock::duration elapsed{};

        auto t = [](bool& out,
                    std::chrono::steady_clock::duration& e) -> capy::task<> {
            auto start = std::chrono::steady_clock::now();
            auto [ec]  = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::duration<std::int32_t, std::milli>(5));
            e   = std::chrono::steady_clock::now() - start;
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out, elapsed));

        ioc.run();
        BOOST_TEST(timed_out);
        BOOST_TEST(elapsed < std::chrono::seconds(5));
    }

    void testNegativeExtremeDeadline()
    {
        // A hugely negative coarse deadline is already elapsed; the
        // noexcept clamp must not invoke duration_cast UB, and the
        // deadline wins immediately as cond::timeout.
        io_context ioc(Backend);
        bool timed_out = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::seconds(10)),
                std::chrono::duration<std::int64_t, std::ratio<3600>>::min());
            out = (ec == capy::cond::timeout);
        };
        capy::run_async(ioc.get_executor())(t(timed_out));

        ioc.run();
        BOOST_TEST(timed_out);
    }

    void testPositiveExtremeDeadlineInnerWins()
    {
        // hours::max() clamps and arms without overflow UB, saturating
        // to an effectively-infinite deadline. The short inner op wins
        // rather than the deadline firing immediately from a wrapped
        // (past) expiry.
        io_context ioc(Backend);
        bool ok = false;

        auto t = [](bool& out) -> capy::task<> {
            auto [ec] = co_await timeout(
                delay(std::chrono::milliseconds(5)),
                (std::chrono::hours::max)());
            out = !ec;
        };
        capy::run_async(ioc.get_executor())(t(ok));

        ioc.run();
        BOOST_TEST(ok);
    }

    void testInitiationStopRace()
    {
        // Race a short inner delay against a short deadline while a
        // foreign thread requests stop. Every completion must be exactly
        // one of success, cond::timeout, or cond::canceled, and the
        // totals must account for every task. Regression guard for the
        // detached timeout coroutine's executor anchor: before the
        // frame-owned executor copy, the deadline waiter's queued
        // completion posted through a ref into the finished chain's
        // recycled trampoline frame and crashed under exactly this
        // churn.
        constexpr int N = 200;

        for (int iter = 0; iter < 5; ++iter)
        {
            io_context ioc(Backend, 2u); // multi-threaded, not hint 1
            auto ex = ioc.get_executor();

            std::atomic<int> success{0};
            std::atomic<int> timed_out{0};
            std::atomic<int> canceled{0};
            std::atomic<int> other{0};
            std::vector<std::stop_source> srcs(N);

            auto task = [](std::atomic<int>& s, std::atomic<int>& to,
                           std::atomic<int>& c, std::atomic<int>& o,
                           int inner_ms, int dl_ms) -> capy::task<> {
                auto [ec] = co_await timeout(
                    delay(std::chrono::milliseconds(inner_ms)),
                    std::chrono::milliseconds(dl_ms));
                if (!ec)
                    s.fetch_add(1, std::memory_order_relaxed);
                else if (ec == capy::cond::timeout)
                    to.fetch_add(1, std::memory_order_relaxed);
                else if (ec == capy::cond::canceled)
                    c.fetch_add(1, std::memory_order_relaxed);
                else
                    o.fetch_add(1, std::memory_order_relaxed);
            };

            for (int i = 0; i < N; ++i)
                capy::run_async(ex, srcs[i].get_token())(
                    task(success, timed_out, canceled, other,
                        i % 3, (i + 1) % 3)); // staggered 0-2ms each

            std::thread stopper([&] {
                for (int i = 0; i < N; ++i)
                    srcs[i].request_stop();
            });
            std::thread r1([&] { ioc.run(); });
            std::thread r2([&] { ioc.run(); });

            r1.join();
            r2.join();
            stopper.join();

            BOOST_TEST_EQ(other.load(), 0);
            BOOST_TEST_EQ(
                success.load() + timed_out.load() + canceled.load(), N);
        }
    }

    void run()
    {
        testInnerWins();
        testDeadlineWins();
        testTimePointDeadlineWins();
        testParentCancelReturnsCanceled();
        testInnerErrorPassesThrough();
        testInnerExceptionPropagates();
        testSingleThreadedHint();
        testPayloadPreservedOnInnerWin();
        testReadyInnerCompletesWithoutSuspend();
        testReadyInnerCanceledNotRemapped();
        testTimeoutResultHasDefaultPayload();
        testNestedTimeouts();
        testTimeoutAlreadyElapsedDeadline();
        testShutdownWithSuspendedTimeout();
        testTimeoutOverSocket();
        testTypeErasedInnerFullProtocol();
        testTypeErasedDeadlineWins();
        testNestedTimeoutOverTypeErased();
        testTypeErasedWriteFullProtocol();
        testAlreadyStoppedCompletesCanceled();
        testNonIoContextThrows();
        testNarrowRepDeadlineClamp();
        testNegativeExtremeDeadline();
        testPositiveExtremeDeadlineInnerWins();
        testInitiationStopRace();
    }
};

COROSIO_BACKEND_TESTS(timeout_test, "boost.corosio.timeout")

} // namespace boost::corosio
