//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_TIMERS_NT_HPP
#define BOOST_COROSIO_DETAIL_IOCP_TIMERS_NT_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include "src/detail/iocp/timers.hpp"
#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

// NT API type definitions
using NTSTATUS = LONG;

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

class win_timers_nt final : public win_timers
{
    void* iocp_;
    void* waitable_timer_ = nullptr;
    void* wait_packet_ = nullptr;
    NtAssociateWaitCompletionPacketFn nt_associate_;
    NtCancelWaitCompletionPacketFn nt_cancel_;

    win_timers_nt(
        void* iocp,
        long* dispatch_required,
        NtAssociateWaitCompletionPacketFn nt_assoc,
        NtCancelWaitCompletionPacketFn nt_cancel);

public:
    // Returns nullptr if NT APIs unavailable (pre-Windows 8)
    static std::unique_ptr<win_timers_nt> try_create(
        void* iocp, long* dispatch_required);

    ~win_timers_nt();

    win_timers_nt(win_timers_nt const&) = delete;
    win_timers_nt& operator=(win_timers_nt const&) = delete;

    void start() override;
    void stop() override;
    void update_timeout(time_point next_expiry) override;

private:
    void associate_timer();
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_TIMERS_NT_HPP
