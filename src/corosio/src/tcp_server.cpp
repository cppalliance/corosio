//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_server.hpp>

namespace boost {
namespace corosio {

tcp_server::push_aw::push_aw(
    tcp_server& self,
    worker_base& w) noexcept
    : self_(self)
    , w_(w)
{
}

bool
tcp_server::push_aw::await_ready() const noexcept
{
    return false;
}

void
tcp_server::push_aw::await_resume() noexcept
{
    if(self_.waiters_)
    {
        auto* wait = self_.waiters_;
        self_.waiters_ = wait->next;
        wait->w = &w_;
        self_.post_.post(wait->h);
    }
    else
    {
        self_.wv_.push(w_);
    }
}

tcp_server::pop_aw::pop_aw(tcp_server& self) noexcept
    : self_(self)
    , wait_{}
{
}

bool
tcp_server::pop_aw::await_ready() const noexcept
{
    return self_.wv_.idle_ != nullptr;
}

system::result<tcp_server::worker_base&>
tcp_server::pop_aw::await_resume() noexcept
{
    if(wait_.w)
        return *wait_.w;
    return *self_.wv_.try_pop();
}

tcp_server::push_aw
tcp_server::push(worker_base& w)
{
    return push_aw{*this, w};
}

void
tcp_server::push_sync(worker_base& w) noexcept
{
    if(waiters_)
    {
        auto* wait = waiters_;
        waiters_ = wait->next;
        wait->w = &w;
        post_.post(wait->h);
    }
    else
    {
        wv_.push(w);
    }
}

tcp_server::pop_aw
tcp_server::pop()
{
    return pop_aw{*this};
}

capy::task<void>
tcp_server::do_accept(acceptor& acc)
{
    auto st = co_await capy::get_stop_token();
    while(! st.stop_requested())
    {
        auto rv = co_await pop();
        if(rv.has_error())
            continue;
        auto& w = rv.value();
        auto ec = co_await acc.accept(w.sock);
        if(ec)
        {
            co_await push(w);
            continue;
        }
        w.run(launcher{*this, w});
    }
}

system::error_code
tcp_server::bind(endpoint ep)
{
    ports_.emplace_back(ctx_);
    // VFALCO this should return error_code
    ports_.back().listen(ep);
    return {};
}

void
tcp_server::start()
{
    for(auto& t : ports_)
        capy::run_async(post_)(do_accept(t));
}

} // namespace corosio
} // namespace boost
