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

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../../common/benchmark.hpp"

namespace asio = boost::asio;

namespace asio_bench {
namespace {

// Tight create/schedule/cancel/destroy loop. Asio manages timers in a
// per-context ordered list without timerfd, so this is bounded by
// list insertion cost and steady_clock::now() calls.
bench::benchmark_result
bench_schedule_cancel(double duration_s)
{
    perf::print_header("Timer Schedule/Cancel (Asio Coroutines)");

    asio::io_context ioc;
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
// scheduler's timer completion path — Asio passes the nearest expiry as
// the epoll_wait timeout, avoiding a timerfd syscall per fire.
bench::benchmark_result
bench_fire_rate(double duration_s)
{
    perf::print_header("Timer Fire Rate (Asio Coroutines)");

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
// Stresses the timer queue under contention and reveals wake accuracy
// degradation as the number of pending timers grows.
bench::benchmark_result
bench_concurrent_timers(int num_timers, double duration_s)
{
    std::cout << "  Timers: " << num_timers << "\n";

    asio::io_context ioc;
    std::atomic<bool> running{true};
    std::vector<int64_t> fire_counts(num_timers, 0);
    std::vector<perf::statistics> stats(num_timers);

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
                double latency_us = sw.elapsed_us();
                stats[idx].add(latency_us);
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

void
run_timer_benchmarks(
    bench::result_collector& collector, char const* filter, double duration_s)
{
    bool run_all = !filter || std::strcmp(filter, "all") == 0;

    if (run_all || std::strcmp(filter, "schedule_cancel") == 0)
        collector.add(bench_schedule_cancel(duration_s));

    if (run_all || std::strcmp(filter, "fire_rate") == 0)
        collector.add(bench_fire_rate(duration_s));

    if (run_all || std::strcmp(filter, "concurrent") == 0)
    {
        perf::print_header("Concurrent Timers (Asio Coroutines)");
        collector.add(bench_concurrent_timers(10, duration_s));
        collector.add(bench_concurrent_timers(100, duration_s));
        collector.add(bench_concurrent_timers(1000, duration_s));
    }
}

} // namespace asio_bench
