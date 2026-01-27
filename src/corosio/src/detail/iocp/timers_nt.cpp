//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//


#if defined(_WIN32)

#include "src/detail/iocp/timers_nt.hpp"
#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

// NT API type definitions
using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;

using NtCreateWaitCompletionPacketFn = NTSTATUS(NTAPI*)(
    void** WaitCompletionPacketHandle,
    ULONG DesiredAccess,
    void* ObjectAttributes);

using NtAssociateWaitCompletionPacketFn = NTSTATUS(NTAPI*)(
    void* WaitCompletionPacketHandle,
    void* IoCompletionHandle,
    void* TargetObjectHandle,
    void* KeyContext,
    void* ApcContext,
    NTSTATUS IoStatus,
    ULONG_PTR IoStatusInformation,
    BOOLEAN* AlreadySignaled);

using NtCancelWaitCompletionPacketFn = NTSTATUS(NTAPI*)(
    void* WaitCompletionPacketHandle,
    BOOLEAN RemoveSignaledPacket);

win_timers_nt::
win_timers_nt(
    void* iocp,
    long* dispatch_required,
    void* nt_create,
    void* nt_assoc,
    void* nt_cancel)
    : win_timers(dispatch_required)
    , iocp_(iocp)
    , nt_create_wait_completion_packet_(nt_create)
    , nt_associate_wait_completion_packet_(nt_assoc)
    , nt_cancel_wait_completion_packet_(nt_cancel)
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

    auto nt_create = ::GetProcAddress(ntdll, "NtCreateWaitCompletionPacket");
    auto nt_assoc = ::GetProcAddress(ntdll, "NtAssociateWaitCompletionPacket");
    auto nt_cancel = ::GetProcAddress(ntdll, "NtCancelWaitCompletionPacket");

    if (!nt_create || !nt_assoc || !nt_cancel)
        return nullptr;

    auto p = std::unique_ptr<win_timers_nt>(new win_timers_nt(
        iocp, dispatch_required,
        reinterpret_cast<void*>(nt_create),
        reinterpret_cast<void*>(nt_assoc),
        reinterpret_cast<void*>(nt_cancel)));

    if (!p->waitable_timer_)
        return nullptr;

    // Create the wait completion packet
    auto create_fn = reinterpret_cast<NtCreateWaitCompletionPacketFn>(
        p->nt_create_wait_completion_packet_);
    NTSTATUS status = create_fn(&p->wait_packet_, MAXIMUM_ALLOWED, nullptr);
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
    if (wait_packet_ && nt_cancel_wait_completion_packet_)
    {
        auto cancel_fn = reinterpret_cast<NtCancelWaitCompletionPacketFn>(
            nt_cancel_wait_completion_packet_);
        cancel_fn(wait_packet_, TRUE);
    }
}

void
win_timers_nt::
update_timeout(time_point next_expiry)
{
    if (!waitable_timer_)
        return;

    // Cancel pending association
    if (wait_packet_ && nt_cancel_wait_completion_packet_)
    {
        auto cancel_fn = reinterpret_cast<NtCancelWaitCompletionPacketFn>(
            nt_cancel_wait_completion_packet_);
        cancel_fn(wait_packet_, FALSE);
    }

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
    if (!wait_packet_ || !nt_associate_wait_completion_packet_)
        return;

    // Set dispatch flag before associating
    ::InterlockedExchange(dispatch_required_, 1);

    auto assoc_fn = reinterpret_cast<NtAssociateWaitCompletionPacketFn>(
        nt_associate_wait_completion_packet_);

    BOOLEAN already_signaled = FALSE;
    NTSTATUS status = assoc_fn(
        wait_packet_,
        iocp_,
        waitable_timer_,
        this,
        nullptr,
        STATUS_SUCCESS,
        0,
        &already_signaled);

    if (status == STATUS_SUCCESS && already_signaled)
        repost(iocp_);
}

} // namespace boost::corosio::detail

#endif
