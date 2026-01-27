//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//


#if defined(_WIN32)

#include "src/detail/iocp/timers.hpp"
#include "src/detail/iocp/timers_nt.hpp"
#include "src/detail/iocp/timers_thread.hpp"

namespace boost::corosio::detail {

std::unique_ptr<win_timers>
make_win_timers(void* iocp, long* dispatch_required)
{
    // Try NT native API first (Windows 8+)
    if (auto p = win_timers_nt::try_create(iocp, dispatch_required))
        return p;

    // Fall back to dedicated thread
    return std::make_unique<win_timers_thread>(iocp, dispatch_required);
}

} // namespace boost::corosio::detail

#endif
