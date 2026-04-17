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

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/detail/concurrency_hint.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using asio_bench::tcp_socket;

namespace asio_callback_bench {
namespace {

// Echo server: reads then writes back, loops via callbacks
struct echo_server_op : std::enable_shared_from_this<echo_server_op>
{
    tcp_socket& sock;
    char buf[64];

    explicit echo_server_op(tcp_socket& s) : sock(s) {}

    void start()
    {
        do_read();
    }

    void do_read()
    {
        auto self = shared_from_this();
        sock.async_read_some(
            asio::buffer(buf, 64),
            [self](boost::system::error_code ec, std::size_t n) {
                if (ec)
                    return;
                self->do_write(n);
            });
    }

    void do_write(std::size_t n)
    {
        auto self = shared_from_this();
        asio::async_write(
            sock, asio::buffer(buf, n),
            [self](boost::system::error_code ec, std::size_t) {
                if (ec)
                    return;
                self->do_read();
            });
    }
};

// Single sub-request: write 64 bytes, read 64 bytes, decrement counter
struct sub_request_op : std::enable_shared_from_this<sub_request_op>
{
    tcp_socket& client;
    std::atomic<int>& remaining;
    std::function<void()> on_join;
    char send_buf[64] = {};
    char recv_buf[64];

    sub_request_op(
        tcp_socket& c, std::atomic<int>& rem, std::function<void()> join_cb)
        : client(c)
        , remaining(rem)
        , on_join(std::move(join_cb))
    {
    }

    void start()
    {
        auto self = shared_from_this();
        asio::async_write(
            client, asio::buffer(send_buf, 64),
            [self](boost::system::error_code ec, std::size_t) {
                if (ec)
                {
                    self->finish();
                    return;
                }
                self->do_read();
            });
    }

    void do_read()
    {
        auto self = shared_from_this();
        asio::async_read(
            client, asio::buffer(recv_buf, 64),
            [self](boost::system::error_code ec, std::size_t) {
                (void)ec;
                self->finish();
            });
    }

    void finish()
    {
        if (remaining.fetch_sub(1, std::memory_order_release) == 1)
            on_join();
    }
};

struct fork_join_op
{
    asio::io_context& ioc;
    std::vector<tcp_socket>& clients;
    std::vector<tcp_socket>& servers;
    int fan_out;
    bench::state& state;
    std::atomic<int> remaining{0};
    perf::stopwatch sw;

    void start()
    {
        if (!state.running())
        {
            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
            return;
        }

        sw.reset();
        remaining.store(fan_out, std::memory_order_relaxed);

        for (int i = 0; i < fan_out; ++i)
        {
            auto op = std::make_shared<sub_request_op>(
                clients[i], remaining, [this]() { on_join(); });
            op->start();
        }
    }

    void on_join()
    {
        state.latency().add(sw.elapsed_ns());
        state.ops().fetch_add(1, std::memory_order_relaxed);
        start();
    }
};

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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    fork_join_op op{ioc, clients, servers, fan_out, state, {}, {}};

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

struct nested_group_op
{
    asio::io_context& ioc;
    std::vector<tcp_socket>& clients;
    int base_idx;
    int n;
    std::atomic<int>& groups_remaining;
    std::function<void()> on_all_groups_done;
    std::atomic<int> subs_remaining;

    nested_group_op(
        asio::io_context& io,
        std::vector<tcp_socket>& cli,
        int base,
        int count,
        std::atomic<int>& gr,
        std::function<void()> cb)
        : ioc(io)
        , clients(cli)
        , base_idx(base)
        , n(count)
        , groups_remaining(gr)
        , on_all_groups_done(std::move(cb))
        , subs_remaining(0)
    {
    }

    void start()
    {
        subs_remaining.store(n, std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            auto op = std::make_shared<sub_request_op>(
                clients[base_idx + i], subs_remaining,
                [this]() { on_group_done(); });
            op->start();
        }
    }

    void on_group_done()
    {
        if (groups_remaining.fetch_sub(1, std::memory_order_release) == 1)
            on_all_groups_done();
    }
};

struct nested_op
{
    asio::io_context& ioc;
    std::vector<tcp_socket>& clients;
    std::vector<tcp_socket>& servers;
    int groups;
    int subs_per_group;
    bench::state& state;
    std::atomic<int> groups_remaining{0};
    std::vector<std::unique_ptr<nested_group_op>> group_ops;
    perf::stopwatch sw;

    void start()
    {
        if (!state.running())
        {
            for (auto& c : clients)
                c.close();
            for (auto& s : servers)
                s.close();
            return;
        }

        sw.reset();
        groups_remaining.store(groups, std::memory_order_relaxed);
        group_ops.clear();
        group_ops.reserve(groups);

        for (int g = 0; g < groups; ++g)
        {
            group_ops.push_back(
                std::make_unique<nested_group_op>(
                    ioc, clients, g * subs_per_group, subs_per_group,
                    groups_remaining, [this]() { on_join(); }));
            group_ops.back()->start();
        }
    }

    void on_join()
    {
        state.latency().add(sw.elapsed_ns());
        state.ops().fetch_add(1, std::memory_order_relaxed);
        start();
    }
};

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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    nested_op op{ioc,     clients, servers, groups, subs_per_group,
                 state,   {},      {},      {}};

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
    ioc.run();
    stopper.join();

    state.set_elapsed(sw.elapsed_seconds());
}

struct parent_fork_join_op
{
    asio::io_context& ioc;
    std::vector<tcp_socket>& clients;
    std::vector<tcp_socket>& servers;
    int base;
    int fan_out;
    int num_parents;
    bench::state& state;
    std::atomic<int>& parents_done;
    std::atomic<int> remaining;
    perf::stopwatch sw;

    parent_fork_join_op(
        asio::io_context& io,
        std::vector<tcp_socket>& cli,
        std::vector<tcp_socket>& srv,
        int b,
        int fo,
        int np,
        bench::state& st,
        std::atomic<int>& pd)
        : ioc(io)
        , clients(cli)
        , servers(srv)
        , base(b)
        , fan_out(fo)
        , num_parents(np)
        , state(st)
        , parents_done(pd)
        , remaining(0)
    {
    }

    void start()
    {
        if (!state.running())
        {
            if (parents_done.fetch_add(1, std::memory_order_acq_rel) ==
                num_parents - 1)
            {
                for (auto& c : clients)
                    c.close();
                for (auto& s : servers)
                    s.close();
            }
            return;
        }

        sw.reset();
        remaining.store(fan_out, std::memory_order_relaxed);

        for (int i = 0; i < fan_out; ++i)
        {
            auto op = std::make_shared<sub_request_op>(
                clients[base + i], remaining, [this]() { on_join(); });
            op->start();
        }
    }

    void on_join()
    {
        state.latency().add(sw.elapsed_ns());
        state.ops().fetch_add(1, std::memory_order_relaxed);
        start();
    }
};

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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    std::atomic<int> parents_done{0};

    std::vector<std::unique_ptr<parent_fork_join_op>> parent_ops;
    parent_ops.reserve(num_parents);

    for (int p = 0; p < num_parents; ++p)
    {
        parent_ops.push_back(
            std::make_unique<parent_fork_join_op>(
                ioc, clients, servers, p * fan_out, fan_out, num_parents,
                state, parents_done));
        parent_ops.back()->start();
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < fan_out; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    fork_join_op op{ioc, clients, servers, fan_out, state, {}, {}};

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    nested_op op{ioc,     clients, servers, groups, subs_per_group,
                 state,   {},      {},      {}};

    op.start();

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
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
        auto [c, s] = asio_bench::make_socket_pair(ioc);
        clients.push_back(std::move(c));
        servers.push_back(std::move(s));
    }

    for (int i = 0; i < total_subs; ++i)
    {
        auto echo = std::make_shared<echo_server_op>(servers[i]);
        echo->start();
    }

    std::atomic<int> parents_done{0};

    std::vector<std::unique_ptr<parent_fork_join_op>> parent_ops;
    parent_ops.reserve(num_parents);

    for (int p = 0; p < num_parents; ++p)
    {
        parent_ops.push_back(
            std::make_unique<parent_fork_join_op>(
                ioc, clients, servers, p * fan_out, fan_out, num_parents,
                state, parents_done));
        parent_ops.back()->start();
    }

    std::thread stopper([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        state.stop();
    });

    perf::stopwatch sw;
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

} // namespace asio_callback_bench
