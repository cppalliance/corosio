//
// Copyright (c) 2026 Vinnie Falco (vinnie.falco@gmail.com)
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

struct tcp_server::impl
{
    std::mutex join_mutex;
    std::condition_variable join_cv;
    capy::execution_context& ctx;
    std::vector<tcp_acceptor> ports;
    std::stop_source stop;

    explicit impl(capy::execution_context& c) noexcept
        : ctx(c)
    {
    }
};

tcp_server::impl*
tcp_server::make_impl(capy::execution_context& ctx)
{
    return new impl(ctx);
}

tcp_server::~tcp_server()
{
    delete impl_;
}

tcp_server::tcp_server(
    tcp_server&& o) noexcept
    : impl_(std::exchange(o.impl_, nullptr))
    , ex_(std::move(o.ex_))
    , waiters_(std::exchange(o.waiters_, nullptr))
    , idle_head_(std::exchange(o.idle_head_, nullptr))
    , active_head_(std::exchange(o.active_head_, nullptr))
    , active_tail_(std::exchange(o.active_tail_, nullptr))
    , active_accepts_(std::exchange(o.active_accepts_, 0))
    , storage_(std::move(o.storage_))
    , running_(std::exchange(o.running_, false))
{
}

tcp_server&
tcp_server::operator=(tcp_server&& o) noexcept
{
    delete impl_;
    impl_ = std::exchange(o.impl_, nullptr);
    ex_ = std::move(o.ex_);
    waiters_ = std::exchange(o.waiters_, nullptr);
    idle_head_ = std::exchange(o.idle_head_, nullptr);
    active_head_ = std::exchange(o.active_head_, nullptr);
    active_tail_ = std::exchange(o.active_tail_, nullptr);
    active_accepts_ = std::exchange(o.active_accepts_, 0);
    storage_ = std::move(o.storage_);
    running_ = std::exchange(o.running_, false);
    return *this;
}

// Accept loop: wait for idle worker, accept connection, dispatch
capy::task<void>
tcp_server::do_accept(tcp_acceptor& acc)
{
    auto const& env = co_await capy::this_coro::environment;
    while(! env.stop_token.stop_requested())
    {
        // Wait for an idle worker before blocking on accept
        auto& w = co_await pop();
        auto [ec] = co_await acc.accept(w.socket());
        if(ec)
        {
            co_await push(w);
            continue;
        }
        w.run(launcher{*this, w});
    }
}

std::error_code
tcp_server::bind(endpoint ep)
{
    impl_->ports.emplace_back(impl_->ctx);
    auto ec = impl_->ports.back().listen(ep);
    if (ec)
        impl_->ports.pop_back();
    return ec;
}

void
tcp_server::
start()
{
    // Idempotent - only start if not already running
    if(running_)
        return;
    
    // Previous session must be fully stopped before restart
    if(active_accepts_ != 0)
        detail::throw_logic_error(
            "tcp_server::start: previous session not joined");
    
    running_ = true;
    
    impl_->stop = {};  // Fresh stop source
    auto st = impl_->stop.get_token();
    
    active_accepts_ = impl_->ports.size();
    
    // Launch with completion handler that decrements counter
    for(auto& t : impl_->ports)
        capy::run_async(ex_, st, [this]() {
            std::lock_guard lock(impl_->join_mutex);
            if(--active_accepts_ == 0)
                impl_->join_cv.notify_all();
        })(do_accept(t));
}

void
tcp_server::
stop()
{
    // Idempotent - only stop if running
    if(!running_)
        return;
    running_ = false;
    
    // Stop accept loops
    impl_->stop.request_stop();
    
    // Launch cancellation coroutine on server executor
    capy::run_async(ex_, std::stop_token{})(do_stop());
}

void
tcp_server::
join()
{
    std::unique_lock lock(impl_->join_mutex);
    impl_->join_cv.wait(lock, [this] { return active_accepts_ == 0; });
}

capy::task<>
tcp_server::do_stop()
{
    // Running on server executor - safe to iterate active list
    // Just cancel, don't modify list - workers return themselves when done
    for(auto* w = active_head_; w; w = w->next_)
        w->stop_.request_stop();
    co_return;
}

} // namespace boost::corosio
