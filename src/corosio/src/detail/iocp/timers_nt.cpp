//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/timers_nt.hpp"
#include "src/detail/iocp/completion_key.hpp"
#include "src/detail/iocp/windows.hpp"

/*
    NT Wait Completion Packet Timer Implementation
    ==============================================

    This uses undocumented NT APIs to integrate waitable timers directly with
    IOCP, avoiding the need for a dedicated timer thread.

    CRITICAL: THE ASSOCIATION IS ONE-SHOT
    -------------------------------------

    When NtAssociateWaitCompletionPacket associates a timer with IOCP, the
    association is consumed when the timer fires. After the completion packet
    is posted to IOCP, the wait packet is "spent" and must be re-associated
    before it can fire again.

    This means update_timeout() MUST be called after every timer wakeup to
    re-associate the wait packet, even if the timer expiry hasn't changed.
    The scheduler calls update_timeout() unconditionally in do_one() after
    processing expired timers for this reason.

    WHY THIS IMPLEMENTATION IS SLOW
    --------------------------------

    The re-association must happen on every scheduler iteration, even for
    timer-free workloads. This causes ~60% CPU overhead in benchmarks because
    SetWaitableTimer + NtAssociateWaitCompletionPacket are called repeatedly.

    DO NOT OPTIMIZE BY SKIPPING RE-ASSOCIATION
    ------------------------------------------

    It may seem obvious to skip re-association when no timers exist or when the
    expiry hasn't changed. However, skipping breaks the timer mechanism:

    1. Timer fires -> posts key_wake_dispatch to IOCP
    2. do_one() processes the completion, calls process_expired()
    3. If update_timeout() is skipped, the wait packet is not re-associated
    4. Future timers will never fire -> scheduler hangs

    The correct optimization (if needed) would be at the waitable timer level
    (caching due_time to avoid redundant SetWaitableTimer calls), but the
    NtAssociateWaitCompletionPacket call cannot be skipped after any wakeup.
*/

namespace boost::corosio::detail {

constexpr NTSTATUS STATUS_SUCCESS = 0;

using NtCreateWaitCompletionPacketFn = NTSTATUS(NTAPI*)(
    void** WaitCompletionPacketHandle,
    ULONG DesiredAccess,
    void* ObjectAttributes);

win_timers_nt::
win_timers_nt(
    void* iocp,
    long* dispatch_required,
    NtAssociateWaitCompletionPacketFn nt_assoc,
    NtCancelWaitCompletionPacketFn nt_cancel)
    : win_timers(dispatch_required)
    , iocp_(iocp)
    , nt_associate_(nt_assoc)
    , nt_cancel_(nt_cancel)
{
    waitable_timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

std::unique_ptr<win_timers_nt>
win_timers_nt::
try_create(void* iocp, long* dispatch_required)
{
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return nullptr;

    auto nt_create = reinterpret_cast<NtCreateWaitCompletionPacketFn>(
        ::GetProcAddress(ntdll, "NtCreateWaitCompletionPacket"));
    auto nt_assoc = reinterpret_cast<NtAssociateWaitCompletionPacketFn>(
        ::GetProcAddress(ntdll, "NtAssociateWaitCompletionPacket"));
    auto nt_cancel = reinterpret_cast<NtCancelWaitCompletionPacketFn>(
        ::GetProcAddress(ntdll, "NtCancelWaitCompletionPacket"));

    if (!nt_create || !nt_assoc || !nt_cancel)
        return nullptr;

    auto p = std::unique_ptr<win_timers_nt>(new win_timers_nt(
        iocp, dispatch_required, nt_assoc, nt_cancel));

    if (!p->waitable_timer_)
        return nullptr;

    // Create the wait completion packet
    NTSTATUS status = nt_create(&p->wait_packet_, MAXIMUM_ALLOWED, nullptr);
    if (status != STATUS_SUCCESS || !p->wait_packet_)
        return nullptr;

    return p;
}

win_timers_nt::
~win_timers_nt()
{
    if (wait_packet_)
        ::CloseHandle(wait_packet_);
    if (waitable_timer_)
        ::CloseHandle(waitable_timer_);
}

void
win_timers_nt::
start()
{
    associate_timer();
}

void
win_timers_nt::
stop()
{
    nt_cancel_(wait_packet_, TRUE);
}

void
win_timers_nt::
update_timeout(time_point next_expiry)
{
    BOOST_COROSIO_ASSERT(waitable_timer_);

    // Cancel pending association
    nt_cancel_(wait_packet_, FALSE);

    auto now = std::chrono::steady_clock::now();
    LARGE_INTEGER due_time;

    if (next_expiry <= now)
    {
        // Already expired - fire immediately
        due_time.QuadPart = 0;
    }
    else if (next_expiry == time_point::max())
    {
        // No timers - set far future
        due_time.QuadPart = -LONGLONG(49) * 24 * 60 * 60 * 10000000LL;
    }
    else
    {
        // Convert duration to 100ns units (negative = relative)
        auto duration = next_expiry - now;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        due_time.QuadPart = -(ns / 100);
        if (due_time.QuadPart == 0)
            due_time.QuadPart = -1;
    }

    ::SetWaitableTimer(waitable_timer_, &due_time, 0, nullptr, nullptr, FALSE);
    associate_timer();
}

void
win_timers_nt::
associate_timer()
{
    // Set dispatch flag before associating
    ::InterlockedExchange(dispatch_required_, 1);

    BOOLEAN already_signaled = FALSE;
    NTSTATUS status = nt_associate_(
        wait_packet_,
        iocp_,
        waitable_timer_,
        reinterpret_cast<void*>(key_wake_dispatch),
        nullptr,
        STATUS_SUCCESS,
        0,
        &already_signaled);

    if (status == STATUS_SUCCESS && already_signaled)
    {
        ::PostQueuedCompletionStatus(
            static_cast<HANDLE>(iocp_),
            0,
            key_wake_dispatch,
            nullptr);
    }
}

} // namespace boost::corosio::detail

#endif
