//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>

#ifdef _WIN32
#include "detail/win_iocp_scheduler.hpp"
#else
#include "detail/posix_scheduler.hpp"
#endif

#include <thread>

namespace boost {
namespace corosio {

#ifdef _WIN32
using scheduler_type = detail::win_iocp_scheduler;
#else
using scheduler_type = detail::posix_scheduler;
#endif

io_context::
io_context()
    : io_context(std::thread::hardware_concurrency())
{
}

io_context::
io_context(
    unsigned concurrency_hint)
    : sched_(make_service<scheduler_type>(concurrency_hint))
{
}

} // namespace corosio
} // namespace boost
