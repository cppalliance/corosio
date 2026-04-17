//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;

namespace asio_bench {
namespace {

asio::awaitable<void>
increment_task(int64_t& counter)
{
    ++counter;
    co_return;
}

asio::awaitable<void>
atomic_increment_task(std::atomic<int64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

void
bench_single_threaded_post(bench::state& state)
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
            asio::co_spawn(ioc, increment_task(counter), asio::detached);

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

void
bench_multithreaded_scaling(bench::state& state)
{
    int max_threads = static_cast<int>(state.range(0));

    asio::io_context ioc;
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 100000;

    for (int i = 0; i < batch_size; ++i)
        asio::co_spawn(ioc, atomic_increment_task(counter), asio::detached);

    perf::stopwatch sw;

    std::thread feeder([&]() {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(state.duration());

        while (std::chrono::steady_clock::now() < deadline)
        {
            for (int i = 0; i < batch_size; ++i)
                asio::co_spawn(
                    ioc, atomic_increment_task(counter), asio::detached);
            std::this_thread::yield();
        }
        running.store(false, std::memory_order_relaxed);
    });

    std::vector<std::thread> runners;
    runners.reserve(max_threads);
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

void
bench_interleaved_post_run(bench::state& state)
{
    int handlers_per_iteration = 100;

    asio::io_context ioc;
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            asio::co_spawn(ioc, increment_task(counter), asio::detached);

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

void
bench_concurrent_post_run(bench::state& state)
{
    int num_threads = static_cast<int>(state.range(0));

    asio::io_context ioc;
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 10000;

    perf::stopwatch sw;

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&]() {
            while (running.load(std::memory_order_relaxed))
            {
                for (int i = 0; i < batch_size; ++i)
                    asio::co_spawn(
                        ioc, atomic_increment_task(counter), asio::detached);
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

void
bench_single_threaded_lockless(bench::state& state)
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
            asio::co_spawn(ioc, increment_task(counter), asio::detached);

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

void
bench_interleaved_lockless(bench::state& state)
{
    int handlers_per_iteration = 100;

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            asio::co_spawn(ioc, increment_task(counter), asio::detached);

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

} // anonymous namespace

bench::benchmark_suite
make_io_context_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("io_context", F::is_microbenchmark)
        .add("single_threaded", bench_single_threaded_post)
        .add("multithreaded", bench_multithreaded_scaling)
            .args({8})
        .add("interleaved", bench_interleaved_post_run)
        .add("concurrent", bench_concurrent_post_run)
            .args({4})
        .add("single_threaded_lockless", bench_single_threaded_lockless)
        .add("interleaved_lockless", bench_interleaved_lockless);
}

} // namespace asio_bench
