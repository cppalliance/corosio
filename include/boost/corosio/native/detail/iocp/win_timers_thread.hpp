//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_THREAD_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_THREAD_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_timers.hpp>
#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>
#include <thread>

namespace boost::corosio::detail {

class win_timers_thread final : public win_timers
{
    void* iocp_;
    void* waitable_timer_ = nullptr;
    std::thread thread_;
    long shutdown_ = 0;

public:
    win_timers_thread(void* iocp, long* dispatch_required) noexcept;
    ~win_timers_thread();

    win_timers_thread(win_timers_thread const&)            = delete;
    win_timers_thread& operator=(win_timers_thread const&) = delete;

    void start() override;
    void stop() override;
    void update_timeout(time_point next_expiry) override;

private:
    void thread_func();
};

inline win_timers_thread::win_timers_thread(
    void* iocp, long* dispatch_required) noexcept
    : win_timers(dispatch_required)
    , iocp_(iocp)
{
    waitable_timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

inline win_timers_thread::~win_timers_thread()
{
    stop();
    if (waitable_timer_)
        ::CloseHandle(waitable_timer_);
}

inline void
win_timers_thread::start()
{
    if (!waitable_timer_)
        return;

    thread_ = std::thread([this] { thread_func(); });
}

inline void
win_timers_thread::stop()
{
    if (::InterlockedExchange(&shutdown_, 1) == 0)
    {
        // Wake the timer thread by setting timer to fire immediately
        if (waitable_timer_)
        {
            LARGE_INTEGER due_time;
            due_time.QuadPart = 0;
            ::SetWaitableTimer(
                waitable_timer_, &due_time, 0, nullptr, nullptr, FALSE);
        }
    }

    if (thread_.joinable())
        thread_.join();
}

inline void
win_timers_thread::update_timeout(time_point next_expiry)
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
    else if (next_expiry == (time_point::max)())
    {
        // No timers - set far future (max 49 days in 100ns units)
        due_time.QuadPart = -LONGLONG(49) * 24 * 60 * 60 * 10000000LL;
    }
    else
    {
        // Convert duration to 100ns units (negative = relative)
        auto duration = next_expiry - now;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                      .count();
        due_time.QuadPart = -(ns / 100);
        if (due_time.QuadPart == 0)
            due_time.QuadPart = -1; // At least 100ns
    }

    ::SetWaitableTimer(waitable_timer_, &due_time, 0, nullptr, nullptr, FALSE);
}

inline void
win_timers_thread::thread_func()
{
    while (::InterlockedExchangeAdd(&shutdown_, 0) == 0)
    {
        DWORD result = ::WaitForSingleObject(waitable_timer_, INFINITE);
        if (result != WAIT_OBJECT_0)
            break;

        if (::InterlockedExchangeAdd(&shutdown_, 0) != 0)
            break;

        ::InterlockedExchange(dispatch_required_, 1);
        ::PostQueuedCompletionStatus(
            static_cast<HANDLE>(iocp_), 0, key_wake_dispatch, nullptr);
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_THREAD_HPP
