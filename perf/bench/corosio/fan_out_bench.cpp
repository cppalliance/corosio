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
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/native/native_timer.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

template<auto Backend>
capy::task<>
echo_server(corosio::native_tcp_socket<Backend>& sock)
{
    char buf[64];
    for (;;)
    {
        auto [rec, rn] = co_await sock.read_some(capy::mutable_buffer(buf, 64));
        if (rec)
            co_return;
        auto [wec, wn] =
            co_await capy::write(sock, capy::const_buffer(buf, rn));
        if (wec)
            co_return;
    }
}

template<auto Backend>
capy::task<>
sub_request(
    corosio::native_tcp_socket<Backend>& client, std::atomic<int>& remaining)
{
    char send_buf[64] = {};
    char recv_buf[64];

    auto [wec, wn] =
        co_await capy::write(client, capy::const_buffer(send_buf, 64));
    if (wec)
    {
        remaining.fetch_sub(1, std::memory_order_release);
        co_return;
    }

    auto [rec, rn] =
        co_await capy::read(client, capy::mutable_buffer(recv_buf, 64));
    (void)rec;
    (void)rn;
    remaining.fetch_sub(1, std::memory_order_release);
}

// Parent spawns N sub-requests, waits for all N to complete, then repeats
template<auto Backend>
void
bench_fork_join(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int fan_out = static_cast<int>(state.range(0));
    state.counters["fan_out"] = fan_out;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(fan_out);
    servers.reserve(fan_out);

    for (int i = 0; i < fan_out; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    auto parent = [&]() -> capy::task<> {
        timer_type t(ioc);
        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                capy::run_async(ioc.get_executor())(
                    sub_request<Backend>(clients[i], remaining));

            while (remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(parent());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

// Two-level fan-out: parent spawns M groups, each group spawns N sub-requests
template<auto Backend>
void
bench_nested(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int groups         = static_cast<int>(state.range(0));
    int subs_per_group = 4;
    int total_subs     = groups * subs_per_group;

    state.counters["groups"]         = groups;
    state.counters["subs_per_group"] = subs_per_group;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    auto group_task = [&](int base_idx, int n,
                          std::atomic<int>& groups_remaining) -> capy::task<> {
        std::atomic<int> subs_remaining{n};
        for (int i = 0; i < n; ++i)
            capy::run_async(ioc.get_executor())(
                sub_request<Backend>(clients[base_idx + i], subs_remaining));

        timer_type t(ioc);
        while (subs_remaining.load(std::memory_order_acquire) > 0)
        {
            t.expires_after(std::chrono::nanoseconds(0));
            auto [ec] = co_await t.wait();
            (void)ec;
        }

        groups_remaining.fetch_sub(1, std::memory_order_release);
    };

    auto parent = [&]() -> capy::task<> {
        timer_type t(ioc);
        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> groups_remaining{groups};
            for (int g = 0; g < groups; ++g)
                capy::run_async(ioc.get_executor())(group_task(
                    g * subs_per_group, subs_per_group, groups_remaining));

            while (groups_remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(parent());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

// P independent parents each fanning out to N sub-requests
template<auto Backend>
void
bench_concurrent_parents(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int num_parents = static_cast<int>(state.range(0));
    int fan_out     = 16;
    int total_subs  = num_parents * fan_out;

    state.counters["num_parents"] = num_parents;
    state.counters["fan_out"]     = fan_out;

    corosio::native_io_context<Backend> ioc;

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    std::atomic<int> parents_done{0};

    auto parent_task = [&](int parent_idx) -> capy::task<> {
        int base = parent_idx * fan_out;
        timer_type t(ioc);

        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                capy::run_async(ioc.get_executor())(
                    sub_request<Backend>(clients[base + i], remaining));

            while (remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
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
        capy::run_async(ioc.get_executor())(parent_task(p));

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

template<auto Backend>
void
bench_fork_join_lockless(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int fan_out = static_cast<int>(state.range(0));
    state.counters["fan_out"] = fan_out;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts);

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(fan_out);
    servers.reserve(fan_out);

    for (int i = 0; i < fan_out; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    auto parent = [&]() -> capy::task<> {
        timer_type t(ioc);
        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                capy::run_async(ioc.get_executor())(
                    sub_request<Backend>(clients[i], remaining));

            while (remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(parent());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

template<auto Backend>
void
bench_nested_lockless(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int groups         = static_cast<int>(state.range(0));
    int subs_per_group = 4;
    int total_subs     = groups * subs_per_group;

    state.counters["groups"]         = groups;
    state.counters["subs_per_group"] = subs_per_group;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts);

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    auto group_task = [&](int base_idx, int n,
                          std::atomic<int>& groups_remaining) -> capy::task<> {
        std::atomic<int> subs_remaining{n};
        for (int i = 0; i < n; ++i)
            capy::run_async(ioc.get_executor())(
                sub_request<Backend>(clients[base_idx + i], subs_remaining));

        timer_type t(ioc);
        while (subs_remaining.load(std::memory_order_acquire) > 0)
        {
            t.expires_after(std::chrono::nanoseconds(0));
            auto [ec] = co_await t.wait();
            (void)ec;
        }

        groups_remaining.fetch_sub(1, std::memory_order_release);
    };

    auto parent = [&]() -> capy::task<> {
        timer_type t(ioc);
        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> groups_remaining{groups};
            for (int g = 0; g < groups; ++g)
                capy::run_async(ioc.get_executor())(group_task(
                    g * subs_per_group, subs_per_group, groups_remaining));

            while (groups_remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
                (void)ec;
            }
        }

        for (auto& c : clients)
            c.close();
        for (auto& s : servers)
            s.close();
    };

    perf::stopwatch sw;

    capy::run_async(ioc.get_executor())(parent());

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

template<auto Backend>
void
bench_concurrent_parents_lockless(bench::state& state)
{
    using socket_type = corosio::native_tcp_socket<Backend>;
    using timer_type  = corosio::native_timer<Backend>;

    int num_parents = static_cast<int>(state.range(0));
    int fan_out     = 16;
    int total_subs  = num_parents * fan_out;

    state.counters["num_parents"] = num_parents;
    state.counters["fan_out"]     = fan_out;

    corosio::io_context_options opts;
    opts.single_threaded = true;
    corosio::native_io_context<Backend> ioc(opts);

    std::vector<socket_type> clients;
    std::vector<socket_type> servers;
    clients.reserve(total_subs);
    servers.reserve(total_subs);

    for (int i = 0; i < total_subs; ++i)
    {
        auto [c, s] = corosio::test::make_socket_pair<
            socket_type, corosio::native_tcp_acceptor<Backend>>(ioc);
        c.set_option(corosio::native_socket_option::no_delay(true));
        s.set_option(corosio::native_socket_option::no_delay(true));
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
        capy::run_async(ioc.get_executor())(echo_server<Backend>(servers[i]));

    std::atomic<int> parents_done{0};

    auto parent_task = [&](int parent_idx) -> capy::task<> {
        int base = parent_idx * fan_out;
        timer_type t(ioc);

        while (state.running())
        {
            auto lp = state.lap();

            std::atomic<int> remaining{fan_out};
            for (int i = 0; i < fan_out; ++i)
                capy::run_async(ioc.get_executor())(
                    sub_request<Backend>(clients[base + i], remaining));

            while (remaining.load(std::memory_order_acquire) > 0)
            {
                t.expires_after(std::chrono::nanoseconds(0));
                auto [ec] = co_await t.wait();
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
        capy::run_async(ioc.get_executor())(parent_task(p));

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_fan_out_suite()
{
    using F = bench::bench_flags;
    return bench::benchmark_suite("fan_out", F::needs_conntrack_drain)
        .add("fork_join", bench_fork_join<Backend>)
            .args({1, 4, 16, 64})
        .add("fork_join_lockless", bench_fork_join_lockless<Backend>)
            .args({1, 4, 16, 64})
        .add("nested", bench_nested<Backend>)
            .args({4, 16})
        .add("nested_lockless", bench_nested_lockless<Backend>)
            .args({4, 16})
        .add("concurrent_parents", bench_concurrent_parents<Backend>)
            .args({1, 4, 16})
        .add("concurrent_parents_lockless", bench_concurrent_parents_lockless<Backend>)
            .args({1, 4, 16});
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_fan_out_suite)
