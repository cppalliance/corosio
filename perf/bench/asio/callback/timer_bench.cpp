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

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using asio_bench::timer_type;

namespace asio_callback_bench {
namespace {

// Tight create/schedule/cancel/destroy loop. Same timer internals as the
// coroutine variant — isolates timer management cost without coroutine overhead.
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
            timer_type t(ioc.get_executor());
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

struct fire_rate_op
{
    timer_type timer;
    std::atomic<bool>& running;
    int64_t& counter;

    fire_rate_op(asio::io_context& ioc, std::atomic<bool>& r, int64_t& c)
        : timer(ioc.get_executor())
        , running(r)
        , counter(c)
    {
    }

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;
        timer.expires_after(std::chrono::nanoseconds(0));
        timer.async_wait([this](boost::system::error_code ec) {
            if (ec)
                return;
            ++counter;
            start();
        });
    }
};

// Zero-delay timer re-armed from its own callback. Compared against the
// coroutine variant, the difference isolates coroutine suspend/resume overhead.
void
bench_fire_rate(bench::state& state)
{
    asio::io_context ioc;
    std::atomic<bool> running{true};
    int64_t counter = 0;

    fire_rate_op op(ioc, running, counter);

    perf::stopwatch sw;

    op.start();

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
            timer_type t(ioc.get_executor());
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

    fire_rate_op op(ioc, running, counter);

    perf::stopwatch sw;

    op.start();

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

struct concurrent_timer_op
{
    timer_type timer;
    std::atomic<bool>& running;
    std::chrono::microseconds interval;
    int64_t& fire_count;
    perf::statistics& stats;
    perf::stopwatch sw;

    concurrent_timer_op(
        asio::io_context& ioc,
        std::atomic<bool>& r,
        std::chrono::microseconds iv,
        int64_t& fc,
        perf::statistics& st)
        : timer(ioc.get_executor())
        , running(r)
        , interval(iv)
        , fire_count(fc)
        , stats(st)
    {
    }

    void start()
    {
        if (!running.load(std::memory_order_relaxed))
            return;
        sw.reset();
        timer.expires_after(interval);
        timer.async_wait([this](boost::system::error_code ec) {
            if (ec)
                return;
            stats.add(sw.elapsed_ns());
            ++fire_count;
            start();
        });
    }
};

// N timers with staggered intervals (100us-1000us) firing concurrently.
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

    // Each op writes latency directly to state.latency()
    std::vector<std::unique_ptr<concurrent_timer_op>> ops;
    ops.reserve(num_timers);

    perf::stopwatch total_sw;

    for (int i = 0; i < num_timers; ++i)
    {
        auto interval = std::chrono::microseconds(
            100 + (900 * i) / (num_timers > 1 ? num_timers - 1 : 1));
        ops.push_back(
            std::make_unique<concurrent_timer_op>(
                ioc, running, interval, fire_counts[i], state.latency()));
        ops.back()->start();
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

} // namespace asio_callback_bench
