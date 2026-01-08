//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <corosio/socket.hpp>

namespace corosio {

struct socket::ops_state final
{
    struct read_op
        : capy::executor_work
    {
        capy::coro h;
        capy::any_dispatcher d;

        void operator()() override
        {
            d(h).resume();
        }

        void destroy() override
        {
            // do not delete; owned by socket
        }
    };

    static void deleter(ops_state* p)
    {
        delete p;
    }

    read_op rd;
};

socket::
socket(
    capy::service_provider& sp)
    : reactor_(sp.find_service<platform_reactor>())
    , ops_(new ops_state, ops_state::deleter)
{
    assert(reactor_ != nullptr);
}

void
socket::
do_read_some(
    capy::coro h,
    capy::any_dispatcher d)
{
    ++g_io_count;
    ops_->rd.h = h;
    ops_->rd.d = d;
    reactor_->submit(&ops_->rd);
}

} // corosio
