//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/native/native_timer.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

// Tight create/schedule/cancel/destroy loop — dominated by timer service
// internals (mutex, heap insert/remove, timerfd_settime when earliest changes).
template<auto Backend>
void
bench_schedule_cancel(bench::state& state)
{
    using timer_type = corosio::native_timer<Backend>;

    corosio::native_io_context<Backend> ioc;
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
// scheduler's timer completion path without contention.
template<auto Backend>
void
bench_fire_rate(bench::state& state)
{
    using timer_type = corosio::native_timer<Backend>;

    corosio::native_io_context<Backend> ioc;
    std::atomic<bool> running{true};
    int64_t counter = 0;

    auto task = [&]() -> capy::task<> {
        timer_type t(ioc);
        while (running.load(std::memory_order_relaxed))
        {
            t.expires_after(std::chrono::nanoseconds(0));
            auto [ec] = co_await t.wait();
            if (ec)
                co_return;
            ++counter;
        }
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(task());

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
// Stresses the timer heap under contention.
template<auto Backend>
void
bench_concurrent_timers(bench::state& state)
{
    using timer_type = corosio::native_timer<Backend>;

    int num_timers = static_cast<int>(state.range(0));
    state.counters["num_timers"] = num_timers;

    corosio::native_io_context<Backend> ioc;
    std::atomic<bool> running{true};
    std::vector<int64_t> fire_counts(num_timers, 0);

    auto timer_task = [&](int idx,
                          std::chrono::microseconds interval) -> capy::task<> {
        timer_type t(ioc);
        while (running.load(std::memory_order_relaxed))
        {
            perf::stopwatch sw;
            t.expires_after(interval);
            auto [ec] = co_await t.wait();
            if (ec)
                co_return;
            state.latency().add(sw.elapsed_ns());
            ++fire_counts[idx];
        }
    };

    perf::stopwatch total_sw;

    for (int i = 0; i < num_timers; ++i)
    {
        auto interval = std::chrono::microseconds(
            100 + (900 * i) / (num_timers > 1 ? num_timers - 1 : 1));
        capy::run_async(ioc.get_executor())(timer_task(i, interval));
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

template<auto Backend>
bench::benchmark_suite
make_timer_suite()
{
    return bench::benchmark_suite("timer")
        .add("schedule_cancel", bench_schedule_cancel<Backend>)
        .add("fire_rate", bench_fire_rate<Backend>)
        .add("concurrent", bench_concurrent_timers<Backend>)
            .args({10, 100, 1000});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_timer_suite)
