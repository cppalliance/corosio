//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_completion_key.hpp>
#include <boost/corosio/native/detail/iocp/win_windows.hpp>

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

    virtual void start()                                = 0;
    virtual void stop()                                 = 0;
    virtual void update_timeout(time_point next_expiry) = 0;
};

std::unique_ptr<win_timers>
make_win_timers(void* iocp, long* dispatch_required);

} // namespace boost::corosio::detail

// Include concrete implementations needed by make_win_timers
#include <boost/corosio/native/detail/iocp/win_timers_nt.hpp>
#include <boost/corosio/native/detail/iocp/win_timers_thread.hpp>

namespace boost::corosio::detail {

inline std::unique_ptr<win_timers>
make_win_timers(void* iocp, long* dispatch_required)
{
    // Thread-based is faster; NT API requires one-shot re-association per
    // wakeup which tanks performance. See timers_nt.hpp for details.
    return std::make_unique<win_timers_thread>(iocp, dispatch_required);

#if 0
    // NT native API (Windows 8+)
    if (auto p = win_timers_nt::try_create(iocp, dispatch_required))
        return p;
#endif
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_TIMERS_HPP
