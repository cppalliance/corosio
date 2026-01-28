//
// Copyright (c) 2026 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/tcp_server.hpp>

namespace boost::corosio {

// Accept loop: wait for idle worker, accept connection, dispatch
capy::task<void>
tcp_server::do_accept(acceptor& acc)
{
    auto st = co_await capy::this_coro::stop_token;
    while(! st.stop_requested())
    {
        // Wait for an idle worker before blocking on accept
        auto rv = co_await pop();
        if(rv.has_error())
            continue;
        auto& w = rv.value();
        auto [ec] = co_await acc.accept(w.socket());
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
tcp_server::
start()
{
    for(auto& t : ports_)
        capy::run_async(ex_)(do_accept(t));
}

} // namespace boost::corosio
