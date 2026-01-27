//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//


#if defined(_WIN32)

#include "src/detail/iocp/timers_thread.hpp"
#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

win_timers_thread::
win_timers_thread(void* iocp, long* dispatch_required) noexcept
    : win_timers(dispatch_required)
    , iocp_(iocp)
{
    waitable_timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

win_timers_thread::
~win_timers_thread()
{
    stop();
    if (waitable_timer_)
        ::CloseHandle(waitable_timer_);
}

void
win_timers_thread::
start()
{
    if (!waitable_timer_)
        return;

    thread_ = std::thread([this] { thread_func(); });
}

void
win_timers_thread::
stop()
{
    if (::InterlockedExchange(&shutdown_, 1) == 0)
    {
        // Wake the timer thread by setting timer to fire immediately
        if (waitable_timer_)
        {
            LARGE_INTEGER due_time;
            due_time.QuadPart = 0;
            ::SetWaitableTimer(waitable_timer_, &due_time, 0, nullptr, nullptr, FALSE);
        }
    }

    if (thread_.joinable())
        thread_.join();
}

void
win_timers_thread::
update_timeout(time_point next_expiry)
{
    if (!waitable_timer_)
        return;

    auto now = std::chrono::steady_clock::now();
    LARGE_INTEGER due_time;

    if (next_expiry <= now)
    {
        // Already expired - fire immediately
        due_time.QuadPart = 0;
    }
    else if (next_expiry == time_point::max())
    {
        // No timers - set far future (max 49 days in 100ns units)
        due_time.QuadPart = -LONGLONG(49) * 24 * 60 * 60 * 10000000LL;
    }
    else
    {
        // Convert duration to 100ns units (negative = relative)
        auto duration = next_expiry - now;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        due_time.QuadPart = -(ns / 100);
        if (due_time.QuadPart == 0)
            due_time.QuadPart = -1; // At least 100ns
    }

    ::SetWaitableTimer(waitable_timer_, &due_time, 0, nullptr, nullptr, FALSE);
}

void
win_timers_thread::
thread_func()
{
    while (::InterlockedExchangeAdd(&shutdown_, 0) == 0)
    {
        DWORD result = ::WaitForSingleObject(waitable_timer_, INFINITE);
        if (result != WAIT_OBJECT_0)
            break;

        if (::InterlockedExchangeAdd(&shutdown_, 0) != 0)
            break;

        ::InterlockedExchange(dispatch_required_, 1);
        repost(iocp_);
    }
}

} // namespace boost::corosio::detail

#endif
