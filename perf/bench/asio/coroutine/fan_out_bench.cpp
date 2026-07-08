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

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

namespace asio_bench {
namespace {

asio::awaitable<void, executor_type>
echo_server(tcp_socket& sock)
{
    char buf[64];
    try
    {
        for (;;)
        {
            auto n = co_await sock.async_read_some(
                asio::buffer(buf, 64), asio::deferred);
            co_await asio::async_write(
                sock, asio::buffer(buf, n), asio::deferred);
        }
    }
    catch (std::exception const&)
    {
    }
}

// Notifies a parked timer (expires_at(time_point::max())) when all
// children have arrived: the last arriver's cancel() is the wakeup.
// Mirrors corosio's fan_out_latch for readability parity between the
// two benchmark sides.
struct fan_out_notifier
{
    std::atomic<int>& remaining;
    timer_type& timer;

    void arrive()
    {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            timer.cancel();
    }
};

asio::awaitable<void, executor_type>
sub_request(tcp_socket& client, fan_out_notifier notifier)
{
    char send_buf[64] = {};
    char recv_buf[64];

    try
    {
        co_await asio::async_write(
            client, asio::buffer(send_buf, 64), asio::deferred);
        co_await asio::async_read(
            client, asio::buffer(recv_buf, 64), asio::deferred);
    }
    catch (std::exception const&)
    {
    }

    notifier.arrive();
}

// Parent spawns N sub-requests, waits for all N to complete, then repeats
void
bench_fork_join(bench::state& state)
{
    int fan_out = static_cast<int>(state.range(0));
    state.counters["fan_out"] = fan_out;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(fan_out);
    servers.reserve(fan_out);

    for (int i = 0; i < fan_out; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};

    auto parent = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        // Park the timer at "never"; cancel() leaves the expiry intact,
        // so one arm covers every lap. The last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                asio::co_spawn(
                    ioc,
                    sub_request(clients[i], fan_out_notifier{remaining, t}),
                    asio::detached);

            // Guard closes the cancel-before-wait race: single run
            // thread, so no child can cancel between the load and
            // async_wait registering.
            while (remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, parent(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

// Two-level fan-out: parent spawns M groups, each group spawns N sub-requests
void
bench_nested(bench::state& state)
{
    int groups         = static_cast<int>(state.range(0));
    int subs_per_group = 4;
    int total_subs     = groups * subs_per_group;

    state.counters["groups"]         = groups;
    state.counters["subs_per_group"] = subs_per_group;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};

    auto group_task = [&](int base_idx, int n,
                          fan_out_notifier groups_notifier)
        -> asio::awaitable<void, executor_type> {
        std::atomic<int> subs_remaining{n};
        timer_type t(ioc);
        // Park the timer at "never"; the last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        for (int i = 0; i < n; ++i)
            asio::co_spawn(
                ioc,
                sub_request(
                    clients[base_idx + i], fan_out_notifier{subs_remaining, t}),
                asio::detached);

        // Guard closes the cancel-before-wait race: single run thread,
        // so no child can cancel between the load and async_wait
        // registering.
        while (subs_remaining.load(std::memory_order_acquire) > 0)
        {
            auto [ec] = co_await t.async_wait(asio::as_tuple(asio::deferred));
            (void)ec;
        }

        groups_notifier.arrive();
    };

    auto parent = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        // Groups wait concurrently, so each group_task carries its own
        // timer; this one only notifies on all-groups-done. cancel()
        // leaves the expiry intact, so one arm covers every lap.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> groups_remaining{groups};
            for (int g = 0; g < groups; ++g)
                asio::co_spawn(
                    ioc,
                    group_task(
                        g * subs_per_group, subs_per_group,
                        fan_out_notifier{groups_remaining, t}),
                    asio::detached);

            while (groups_remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, parent(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

// P independent parents each fanning out to N sub-requests
void
bench_concurrent_parents(bench::state& state)
{
    int num_parents = static_cast<int>(state.range(0));
    int fan_out     = 16;
    int total_subs  = num_parents * fan_out;

    state.counters["num_parents"] = num_parents;
    state.counters["fan_out"]     = fan_out;

    asio::io_context ioc;

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};
    std::atomic<int> parents_done{0};

    auto parent_task =
        [&](int parent_idx) -> asio::awaitable<void, executor_type> {
        int base = parent_idx * fan_out;
        timer_type t(ioc);
        // Park the timer at "never"; cancel() leaves the expiry intact,
        // so one arm covers every lap. The last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());

        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                asio::co_spawn(
                    ioc,
                    sub_request(
                        clients[base + i], fan_out_notifier{remaining, t}),
                    asio::detached);

            // Guard closes the cancel-before-wait race: single run
            // thread, so no child can cancel between the load and
            // async_wait registering.
            while (remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        if (parents_done.fetch_add(1, std::memory_order_acq_rel) ==
            num_parents - 1)
        {
            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
        }
    };

    perf::stopwatch sw;

    for (int p = 0; p < num_parents; ++p)
        asio::co_spawn(ioc, parent_task(p), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

void
bench_fork_join_lockless(bench::state& state)
{
    int fan_out = static_cast<int>(state.range(0));
    state.counters["fan_out"] = fan_out;

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(fan_out);
    servers.reserve(fan_out);

    for (int i = 0; i < fan_out; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};

    auto parent = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        // Park the timer at "never"; cancel() leaves the expiry intact,
        // so one arm covers every lap. The last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                asio::co_spawn(
                    ioc,
                    sub_request(clients[i], fan_out_notifier{remaining, t}),
                    asio::detached);

            // Guard closes the cancel-before-wait race: single run
            // thread, so no child can cancel between the load and
            // async_wait registering.
            while (remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, parent(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

void
bench_nested_lockless(bench::state& state)
{
    int groups         = static_cast<int>(state.range(0));
    int subs_per_group = 4;
    int total_subs     = groups * subs_per_group;

    state.counters["groups"]         = groups;
    state.counters["subs_per_group"] = subs_per_group;

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};

    auto group_task = [&](int base_idx, int n,
                          fan_out_notifier groups_notifier)
        -> asio::awaitable<void, executor_type> {
        std::atomic<int> subs_remaining{n};
        timer_type t(ioc);
        // Park the timer at "never"; the last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        for (int i = 0; i < n; ++i)
            asio::co_spawn(
                ioc,
                sub_request(
                    clients[base_idx + i], fan_out_notifier{subs_remaining, t}),
                asio::detached);

        // Guard closes the cancel-before-wait race: single run thread,
        // so no child can cancel between the load and async_wait
        // registering.
        while (subs_remaining.load(std::memory_order_acquire) > 0)
        {
            auto [ec] = co_await t.async_wait(asio::as_tuple(asio::deferred));
            (void)ec;
        }

        groups_notifier.arrive();
    };

    auto parent = [&]() -> asio::awaitable<void, executor_type> {
        timer_type t(ioc);
        // Groups wait concurrently, so each group_task carries its own
        // timer; this one only notifies on all-groups-done. cancel()
        // leaves the expiry intact, so one arm covers every lap.
        t.expires_at(std::chrono::steady_clock::time_point::max());
        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> groups_remaining{groups};
            for (int g = 0; g < groups; ++g)
                asio::co_spawn(
                    ioc,
                    group_task(
                        g * subs_per_group, subs_per_group,
                        fan_out_notifier{groups_remaining, t}),
                    asio::detached);

            while (groups_remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    asio::co_spawn(ioc, parent(), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

void
bench_concurrent_parents_lockless(bench::state& state)
{
    int num_parents = static_cast<int>(state.range(0));
    int fan_out     = 16;
    int total_subs  = num_parents * fan_out;

    state.counters["num_parents"] = num_parents;
    state.counters["fan_out"]     = fan_out;

    asio::io_context ioc(BOOST_ASIO_CONCURRENCY_HINT_UNSAFE);

    std::vector<tcp_socket> clients;
    std::vector<tcp_socket> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        asio::co_spawn(ioc, echo_server(servers[i]), asio::detached);

    std::atomic<bool> running{true};
    std::atomic<int> parents_done{0};

    auto parent_task =
        [&](int parent_idx) -> asio::awaitable<void, executor_type> {
        int base = parent_idx * fan_out;
        timer_type t(ioc);
        // Park the timer at "never"; cancel() leaves the expiry intact,
        // so one arm covers every lap. The last child's cancel() is the
        // wakeup.
        t.expires_at(std::chrono::steady_clock::time_point::max());

        while (running.load(std::memory_order_relaxed))
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                asio::co_spawn(
                    ioc,
                    sub_request(
                        clients[base + i], fan_out_notifier{remaining, t}),
                    asio::detached);

            // Guard closes the cancel-before-wait race: single run
            // thread, so no child can cancel between the load and
            // async_wait registering.
            while (remaining.load(std::memory_order_acquire) > 0)
            {
                auto [ec] =
                    co_await t.async_wait(asio::as_tuple(asio::deferred));
                (void)ec;
            }
        }

        if (parents_done.fetch_add(1, std::memory_order_acq_rel) ==
            num_parents - 1)
        {
            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
        }
    };

    perf::stopwatch sw;

    for (int p = 0; p < num_parents; ++p)
        asio::co_spawn(ioc, parent_task(p), asio::detached);

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

} // anonymous namespace

bench::benchmark_suite
make_fan_out_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("fan_out", F::needs_conntrack_drain)
        .add("fork_join", bench_fork_join)
            .args({1, 4, 16, 64})
        .add("fork_join_lockless", bench_fork_join_lockless)
            .args({1, 4, 16, 64})
        .add("nested", bench_nested)
            .args({4, 16})
        .add("nested_lockless", bench_nested_lockless)
            .args({4, 16})
        .add("concurrent_parents", bench_concurrent_parents)
            .args({1, 4, 16})
        .add("concurrent_parents_lockless", bench_concurrent_parents_lockless)
            .args({1, 4, 16});
}

} // namespace asio_bench
