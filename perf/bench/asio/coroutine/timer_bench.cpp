//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"
#include "../socket_utils.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;

namespace asio_bench {
namespace {

// Tight create/schedule/cancel/destroy loop. Asio manages timers in a
// per-context ordered list without timerfd, so this is bounded by
// list insertion cost and steady_clock::now() calls.
void
bench_schedule_cancel(bench::state& state)
{
    asio::io_context ioc;
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
        {
            timer_type t(ioc);
            t.expires_after(std::chrono::hours(1));
            t.cancel();
            ++counter;
        }

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Single coroutine firing a zero-delay timer in a tight loop. Measures the
// scheduler's timer completion path — Asio passes the nearest expiry as
// the epoll_wait timeout, avoiding a timerfd syscall per fire.
void
bench_fire_rate(bench::state& state)
{
    asio::io_context ioc;
    std::atomic<bool> running{true};
    int64_t counter = 0;

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                t.expires_after(std::chrono::nanoseconds(0));
                co_await t.async_wait(asio::deferred);
                ++counter;
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

void
bench_schedule_cancel_lockless(bench::state& state)
{
    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
        {
            timer_type t(ioc);
            t.expires_after(std::chrono::hours(1));
            t.cancel();
            ++counter;
        }

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

void
bench_fire_rate_lockless(bench::state& state)
{
    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    std::atomic<bool> running{true};
    int64_t counter = 0;

    auto task = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                t.expires_after(std::chrono::nanoseconds(0));
                co_await t.async_wait(asio::deferred);
                ++counter;
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, task(), asio::detached);

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// N timers with staggered intervals (100us–1000us) firing concurrently.
// Stresses the timer queue under contention and reveals wake accuracy
// degradation as the number of pending timers grows.
void
bench_concurrent_timers(bench::state& state)
{
    int num_timers = static_cast<int>(state.range(0));
    state.counters["num_timers"] = num_timers;

    asio::io_context ioc;
    std::atomic<bool> running{true};
    std::vector<int64_t> fire_counts(num_timers, 0);

    auto timer_task = [&](int idx, std::chrono::microseconds interval)
        -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        try
        {
            while (running.load(std::memory_order_relaxed))
            {
                perf::stopwatch sw;
                t.expires_after(interval);
                co_await t.async_wait(asio::deferred);
                state.latency().add(sw.elapsed_ns());
                ++fire_counts[idx];
            }
        }
        catch (std::exception const&)
        {
        }
    };

    perf::stopwatch total_sw;

    for (int i = 0; i < num_timers; ++i)
    {
        auto interval = std::chrono::microseconds(
            100 + (900 * i) / (num_timers > 1 ? num_timers - 1 : 1));
        asio::co_spawn(ioc, timer_task(i, interval), asio::detached);
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(total_sw.elapsed_seconds());

    int64_t total_fires = 0;
    for (auto c : fire_counts)
        total_fires += c;
    state.add_items(total_fires);
}

} // anonymous namespace

bench::benchmark_suite
make_timer_suite()
{
    return bench::benchmark_suite("timer")
        .add("schedule_cancel", bench_schedule_cancel)
        .add("schedule_cancel_lockless", bench_schedule_cancel_lockless)
        .add("fire_rate", bench_fire_rate)
        .add("fire_rate_lockless", bench_fire_rate_lockless)
        .add("concurrent", bench_concurrent_timers)
            .args({10, 100, 1000});
}

} // namespace asio_bench
