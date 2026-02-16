//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP
#define BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/completion_key.hpp"
#include "src/detail/iocp/windows.hpp"

#include <chrono>
#include <memory>

namespace boost::corosio::detail {

/** Abstract interface for timer wakeup mechanisms.

    Posts key_wake_dispatch to the IOCP to trigger timer processing.
*/
class win_timers
{
protected:
    long* dispatch_required_;

public:
    using time_point = std::chrono::steady_clock::time_point;

    explicit win_timers(long* dispatch_required) noexcept
        : dispatch_required_(dispatch_required)
    {
    }

    virtual ~win_timers() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void update_timeout(time_point next_expiry) = 0;
};

std::unique_ptr<win_timers>
make_win_timers(void* iocp, long* dispatch_required);

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP
