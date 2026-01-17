//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_TIMERS_NT_HPP
#define BOOST_COROSIO_DETAIL_WIN_TIMERS_NT_HPP

#include "src/detail/win/timers.hpp"

namespace boost {
namespace corosio {
namespace detail {

// Timer wakeup using NtAssociateWaitCompletionPacket (Windows 8+).
// Only sees void* iocp and long* dispatch_required.
class win_timers_nt final : public win_timers
{
    void* iocp_;
    long* dispatch_required_;
    void* waitable_timer_ = nullptr;
    void* wait_packet_ = nullptr;

    // Dynamically loaded NT API function pointers
    void* nt_create_wait_completion_packet_;
    void* nt_associate_wait_completion_packet_;
    void* nt_cancel_wait_completion_packet_;

    win_timers_nt(
        void* iocp,
        long* dispatch_required,
        void* nt_create,
        void* nt_assoc,
        void* nt_cancel);

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

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
