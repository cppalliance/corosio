//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP
#define BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP


#if defined(_WIN32)

#include "src/detail/iocp/completion_key.hpp"

#include <chrono>
#include <memory>

namespace boost::corosio::detail {

/** Abstract interface for timer wakeup mechanisms.

    Derives from completion_key so the timer object itself serves
    as the IOCP completion key when posting wakeups.
*/
class win_timers : public completion_key
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

    result on_completion(
        win_scheduler&,
        DWORD,
        DWORD,
        LPOVERLAPPED) override
    {
        ::InterlockedExchange(dispatch_required_, 1);
        return result::continue_loop;
    }
};

std::unique_ptr<win_timers> make_win_timers(
    void* iocp, long* dispatch_required);

} // namespace boost::corosio::detail

#endif // _WIN32

#endif // BOOST_COROSIO_DETAIL_IOCP_TIMERS_HPP
