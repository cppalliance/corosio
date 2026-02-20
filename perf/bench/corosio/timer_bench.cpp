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
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/benchmark.hpp"
#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

// Tight create/schedule/cancel/destroy loop — dominated by timer service
// internals (mutex, heap insert/remove, timerfd_settime when earliest changes).
// Low throughput here points to lock contention or excessive syscalls.
template<auto Backend>
bench::benchmark_result
bench_schedule_cancel(double duration_s)
{
    using timer_type = corosio::native_timer<Backend>;

    perf::print_header("Timer Schedule/Cancel (Corosio)");

    corosio::native_io_context<Backend> ioc;
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(duration_s);

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

    double elapsed     = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>(counter) / elapsed;

    std::cout << "  Timers:      " << counter << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << perf::format_rate(ops_per_sec) << "\n";

    return bench::benchmark_result("schedule_cancel")
        .add("timers", static_cast<double>(counter))
        .add("elapsed_s", elapsed)
        .add("ops_per_sec", ops_per_sec);
}

// Single coroutine firing a zero-delay timer in a tight loop. Measures the
// scheduler's timer completion path without contention — expiry update, epoll
// wakeup, and handler dispatch all contribute to the per-fire cost.
template<auto Backend>
bench::benchmark_result
bench_fire_rate(double duration_s)
{
    using timer_type = corosio::native_timer<Backend>;

    perf::print_header("Timer Fire Rate (Corosio)");

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
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    timer.join();

    double elapsed     = sw.elapsed_seconds();
    double ops_per_sec = static_cast<double>(counter) / elapsed;

    std::cout << "  Fires:       " << counter << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "  Throughput:  " << perf::format_rate(ops_per_sec) << "\n";

    return bench::benchmark_result("fire_rate")
        .add("fires", static_cast<double>(counter))
        .add("elapsed_s", elapsed)
        .add("ops_per_sec", ops_per_sec);
}

// N timers with staggered intervals (100us–1000us) firing concurrently.
// Stresses the timer heap under contention and reveals wake accuracy
// degradation as the number of pending timers grows.
template<auto Backend>
bench::benchmark_result
bench_concurrent_timers(int num_timers, double duration_s)
{
    using timer_type = corosio::native_timer<Backend>;

    std::cout << "  Timers: " << num_timers << "\n";

    corosio::native_io_context<Backend> ioc;
    std::atomic<bool> running{true};
    std::vector<int64_t> fire_counts(num_timers, 0);
    std::vector<perf::statistics> stats(num_timers);

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
            double latency_us = sw.elapsed_us();
            stats[idx].add(latency_us);
            ++fire_counts[idx];
        }
    };

    perf::stopwatch total_sw;

    for (int i = 0; i < num_timers; ++i)
    {
        // Stagger intervals from 100us to 1000us
        auto interval = std::chrono::microseconds(
            100 + (900 * i) / (num_timers > 1 ? num_timers - 1 : 1));
        capy::run_async(ioc.get_executor())(timer_task(i, interval));
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    double elapsed = total_sw.elapsed_seconds();

    int64_t total_fires = 0;
    for (auto c : fire_counts)
        total_fires += c;

    double fires_per_sec = static_cast<double>(total_fires) / elapsed;

    double total_mean = 0;
    double total_p99  = 0;
    for (auto& s : stats)
    {
        total_mean += s.mean();
        total_p99 += s.p99();
    }

    std::cout << "    Total fires: " << total_fires << "\n";
    std::cout << "    Elapsed: " << std::fixed << std::setprecision(3)
              << elapsed << " s\n";
    std::cout << "    Throughput: " << perf::format_rate(fires_per_sec) << "\n";
    std::cout << "    Avg mean latency: "
              << perf::format_latency(total_mean / num_timers) << "\n";
    std::cout << "    Avg p99 latency: "
              << perf::format_latency(total_p99 / num_timers) << "\n\n";

    return bench::benchmark_result("concurrent_" + std::to_string(num_timers))
        .add("num_timers", num_timers)
        .add("total_fires", static_cast<double>(total_fires))
        .add("fires_per_sec", fires_per_sec)
        .add("avg_mean_latency_us", total_mean / num_timers)
        .add("avg_p99_latency_us", total_p99 / num_timers);
}

} // anonymous namespace

template<auto Backend>
void
run_timer_benchmarks(
    perf::context_factory factory,
    bench::result_collector& collector,
    char const* filter,
    double duration_s)
{
    (void)factory;
    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    if (run_all || std::strcmp(filter, "schedule_cancel") == 0)
        collector.add(bench_schedule_cancel<Backend>(duration_s));

    if (run_all || std::strcmp(filter, "fire_rate") == 0)
        collector.add(bench_fire_rate<Backend>(duration_s));

    if (run_all || std::strcmp(filter, "concurrent") == 0)
    {
        perf::print_header("Concurrent Timers (Corosio)");
        collector.add(bench_concurrent_timers<Backend>(10, duration_s));
        collector.add(bench_concurrent_timers<Backend>(100, duration_s));
        collector.add(bench_concurrent_timers<Backend>(1000, duration_s));
    }
}

} // namespace corosio_bench

COROSIO_BENCH_INSTANTIATE(void corosio_bench::run_timer_benchmarks)
