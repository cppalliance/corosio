//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_server.hpp>
#include <boost/corosio/detail/except.hpp>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace boost::corosio {

tcp_server::worker_base::worker_base()  = default;
tcp_server::worker_base::~worker_base() = default;

struct tcp_server::impl
{
    std::mutex join_mutex;
    std::condition_variable join_cv;
    capy::execution_context& ctx;
    std::vector<tcp_acceptor> ports;
    std::stop_source stop;
    // Guarded by join_mutex. Each listener is closed exactly once after
    // the server is gone: by its loop's completion if the loop was still
    // running when the server went, otherwise by the departing server.
    std::vector<bool> loop_running;
    bool discarded = false;

    explicit impl(capy::execution_context& c) noexcept : ctx(c) {}
};

tcp_server::state*
tcp_server::make_state(
    capy::execution_context& ctx, capy::any_executor const& ex)
{
    auto* i = new impl(ctx);
    try
    {
        return new state(i, ex);
    }
    catch (...)
    {
        delete i;
        throw;
    }
}

void
tcp_server::release(state* s) noexcept
{
    if (s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        delete s->impl_;
        delete s;
    }
}

void
tcp_server::discard() noexcept
{
    if (!st_)
        return;
    auto& im = *st_->impl_;
    std::vector<std::size_t> idle;
    {
        std::lock_guard lock(im.join_mutex);
        im.discarded = true;
        for (std::size_t i = 0; i < im.ports.size(); ++i)
            if (i >= im.loop_running.size() || !im.loop_running[i])
                idle.push_back(i);
    }
    for (auto i : idle)
        im.ports[i].close();
    stop();
    release(std::exchange(st_, nullptr));
}

tcp_server::~tcp_server()
{
    discard();
}

tcp_server::tcp_server(tcp_server&& o) noexcept
    : st_(std::exchange(o.st_, nullptr))
{
}

tcp_server&
tcp_server::operator=(tcp_server&& o) noexcept
{
    if (this != &o)
    {
        discard();
        st_ = std::exchange(o.st_, nullptr);
    }
    return *this;
}

// Accept loop: wait for idle worker, accept connection, dispatch
capy::task<void>
tcp_server::do_accept(state_ref st, tcp_acceptor& acc)
{
    // Analyzer can't trace value through coroutine await_transform
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    auto env = co_await capy::this_coro::environment;
    while (!env->stop_token.stop_requested())
    {
        // Wait for an idle worker before blocking on accept
        auto* w = co_await pop_awaitable{*st};
        if (!w)
            break;
        w->holds_.store(1, std::memory_order_relaxed);
        auto [ec] = co_await acc.accept(w->socket());
        if (ec)
        {
            co_await push_awaitable{*st, *w};
            continue;
        }
        w->holds_.fetch_add(1, std::memory_order_relaxed);
        w->run(launcher{*st, *w});
        st->drop_hold(*w);
    }
}

std::error_code
tcp_server::bind(endpoint ep)
{
    try
    {
        st_->impl_->ports.emplace_back(st_->impl_->ctx, ep);
        return {};
    }
    catch (std::system_error const& e)
    {
        return e.code();
    }
}

endpoint
tcp_server::local_endpoint(std::size_t index) const noexcept
{
    if (!st_ || index >= st_->impl_->ports.size())
        return endpoint{};
    return st_->impl_->ports[index].local_endpoint();
}

void
tcp_server::start()
{
    // Idempotent - only start if not already running
    if (st_->running)
        return;

    auto& im = *st_->impl_;
    {
        // Previous session must be fully stopped before restart
        std::lock_guard lock(im.join_mutex);
        if (st_->active_accepts != 0)
            detail::throw_logic_error(
                "tcp_server::start: previous session not joined");
        st_->active_accepts = im.ports.size();
        im.loop_running.assign(im.ports.size(), true);
    }
    {
        std::lock_guard<std::mutex> lock(st_->mutex);
        st_->stopping = false;
    }

    st_->running = true;
    im.stop      = {}; // Fresh stop source
    auto token   = im.stop.get_token();

    // The completion runs once the loop's frame is done with its
    // listener, so a close from here cannot race the loop.
    for (std::size_t i = 0; i < im.ports.size(); ++i)
        capy::run_async(st_->ex, token, [st = state_ref(st_), i]() {
            auto& im   = *st->impl_;
            bool close = false;
            {
                std::lock_guard lock(im.join_mutex);
                im.loop_running[i] = false;
                close              = im.discarded;
                if (--st->active_accepts == 0)
                    im.join_cv.notify_all();
            }
            if (close)
                im.ports[i].close();
        })(do_accept(state_ref(st_), im.ports[i]));
}

void
tcp_server::stop()
{
    // Idempotent - only stop if running
    if (!st_ || !st_->running)
        return;
    st_->running = false;

    // Stop accept loops, including those waiting for a worker
    st_->impl_->stop.request_stop();
    st_->stop_waiters();

    // Launch cancellation coroutine on server executor
    capy::run_async(st_->ex, std::stop_token{})(do_stop(state_ref(st_)));
}

void
tcp_server::join()
{
    auto& im = *st_->impl_;
    std::unique_lock lock(im.join_mutex);
    im.join_cv.wait(lock, [this] { return st_->active_accepts == 0; });
}

capy::task<>
tcp_server::do_stop(state_ref st)
{
    // Requested outside the lock: a stop callback may return its worker,
    // which takes the lock.
    std::vector<std::stop_source> active;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        for (auto* w = st->active_head; w; w = w->next_)
            active.push_back(w->stop_);
    }
    for (auto& s : active)
        s.request_stop();
    co_return;
}

} // namespace boost::corosio
