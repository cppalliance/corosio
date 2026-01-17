//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_TIMERS_HPP
#define BOOST_COROSIO_DETAIL_WIN_TIMERS_HPP

#include <chrono>
#include <cstdint>
#include <memory>

namespace boost {
namespace corosio {
namespace detail {

// Completion key posted when timer wakeup fires
constexpr std::uintptr_t timer_key = 3;

// Abstract interface for timer wakeup mechanisms.
// Implementations only receive void* iocp and long* dispatch_required.
class win_timers
{
public:
    using time_point = std::chrono::steady_clock::time_point;

    virtual ~win_timers() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void update_timeout(time_point next_expiry) = 0;
};

// Factory - tries NT native first, falls back to thread
std::unique_ptr<win_timers> make_win_timers(
    void* iocp, long* dispatch_required);

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
