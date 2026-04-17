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

capy::task<>
increment_task(int64_t& counter)
{
    ++counter;
    co_return;
}

capy::task<>
atomic_increment_task(std::atomic<int64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

template<auto Backend>
void
bench_single_threaded_post(bench::state& state)
{
    corosio::native_io_context<Backend> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

template<auto Backend>
void
bench_multithreaded_scaling(bench::state& state)
{
    int max_threads = static_cast<int>(state.range(0));

    corosio::native_io_context<Backend> ioc;
    auto ex = ioc.get_executor();
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 100000;

    for (int i = 0; i < batch_size; ++i)
        capy::run_async(ex)(atomic_increment_task(counter));

    perf::stopwatch sw;

    std::thread feeder([&]() {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(state.duration());

        while (std::chrono::steady_clock::now() < deadline)
        {
            for (int i = 0; i < batch_size; ++i)
                capy::run_async(ex)(atomic_increment_task(counter));
            std::this_thread::yield();
        }
        running.store(false, std::memory_order_relaxed);
    });

    std::vector<std::thread> runners;
    for (int t = 0; t < max_threads; ++t)
        runners.emplace_back([&ioc, &running]() {
            while (running.load(std::memory_order_relaxed))
            {
                ioc.poll();
                ioc.restart();
            }
            ioc.run();
        });

    feeder.join();
    for (auto& t : runners)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter.load());
    state.counters["threads"] = max_threads;
}

template<auto Backend>
void
bench_interleaved_post_run(bench::state& state)
{
    int handlers_per_iteration = 100;

    corosio::native_io_context<Backend> ioc;
    auto ex         = ioc.get_executor();
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

template<auto Backend>
void
bench_concurrent_post_run(bench::state& state)
{
    int num_threads = static_cast<int>(state.range(0));

    corosio::native_io_context<Backend> ioc;
    auto ex = ioc.get_executor();
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 10000;

    perf::stopwatch sw;

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&]() {
            while (running.load(std::memory_order_relaxed))
            {
                for (int i = 0; i < batch_size; ++i)
                    capy::run_async(ex)(atomic_increment_task(counter));
                ioc.poll();
                ioc.restart();
            }
            ioc.run();
        });
    }

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    timer.join();
    for (auto& t : workers)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter.load());
    state.counters["threads"] = num_threads;
}

// Configuration variant: high inline budget
template<auto Backend>
void
bench_high_inline_budget(bench::state& state)
{
    corosio::io_context_options opts;
    opts.inline_budget_max = 64;

    corosio::native_io_context<Backend> ioc(opts);
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Configuration variant: large event buffer
template<auto Backend>
void
bench_large_event_buffer(bench::state& state)
{
    corosio::io_context_options opts;
    opts.max_events_per_poll = 512;

    corosio::native_io_context<Backend> ioc(opts);
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Lockless variant of single_threaded (batch 1000)
template<auto Backend>
void
bench_single_threaded_lockless(bench::state& state)
{
    corosio::io_context_options opts;
    opts.single_threaded = true;

    corosio::native_io_context<Backend> ioc(opts);
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Lockless variant of interleaved (batch 100)
template<auto Backend>
void
bench_interleaved_lockless(bench::state& state)
{
    corosio::io_context_options opts;
    opts.single_threaded = true;

    int handlers_per_iteration = 100;

    corosio::native_io_context<Backend> ioc(opts);
    auto ex         = ioc.get_executor();
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_io_context_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("io_context", F::is_microbenchmark)
        .add("single_threaded", bench_single_threaded_post<Backend>)
        .add("multithreaded", bench_multithreaded_scaling<Backend>)
            .args({8})
        .add("interleaved", bench_interleaved_post_run<Backend>)
        .add("concurrent", bench_concurrent_post_run<Backend>)
            .args({4})
        .add("high_inline_budget", bench_high_inline_budget<Backend>)
        .add("large_event_buffer", bench_large_event_buffer<Backend>)
        .add("single_threaded_lockless", bench_single_threaded_lockless<Backend>)
        .add("interleaved_lockless", bench_interleaved_lockless<Backend>);
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_io_context_suite)
