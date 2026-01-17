//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_TIMERS_THREAD_HPP
#define BOOST_COROSIO_DETAIL_WIN_TIMERS_THREAD_HPP

#include "src/detail/win/timers.hpp"
#include <thread>

namespace boost {
namespace corosio {
namespace detail {

// Timer wakeup using a dedicated thread with a Windows waitable timer.
// Only sees void* iocp and long* dispatch_required.
class win_timers_thread final : public win_timers
{
    void* iocp_;
    long* dispatch_required_;
    void* waitable_timer_ = nullptr;
    std::thread thread_;
    long shutdown_ = 0;

public:
    win_timers_thread(void* iocp, long* dispatch_required) noexcept;
    ~win_timers_thread();

    win_timers_thread(win_timers_thread const&) = delete;
    win_timers_thread& operator=(win_timers_thread const&) = delete;

    void start() override;
    void stop() override;
    void update_timeout(time_point next_expiry) override;

private:
    void thread_func();
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
