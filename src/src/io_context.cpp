//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>

#include "src/win_iocp_scheduler.hpp"

namespace boost {
namespace corosio {

io_context::
io_context()
    : io_context(std::thread::hardware_concurrency())
{
}

io_context::
io_context(
    unsigned concurrency_hint)
    : sched_(use_service<win_iocp_scheduler>())
{
    (void)concurrency_hint;
}

} // namespace corosio
} // namespace boost
