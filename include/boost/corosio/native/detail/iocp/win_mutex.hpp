//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_MUTEX_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_MUTEX_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** Recursive mutex using Windows CRITICAL_SECTION.

    This mutex can be locked multiple times by the same thread.
    Each call to `lock()` or successful `try_lock()` must be
    balanced by a corresponding call to `unlock()`.

    When disabled via `set_enabled(false)`, all locking operations
    become no-ops. This supports single-threaded (lockless) mode
    where cross-thread posting is undefined behavior.

    Satisfies the Lockable named requirement and is compatible
    with `std::lock_guard`, `std::unique_lock`, and `std::scoped_lock`.
*/
class win_mutex
{
public:
    win_mutex()
    {
        ::InitializeCriticalSectionAndSpinCount(&cs_, 0x80000000);
    }

    ~win_mutex()
    {
        ::DeleteCriticalSection(&cs_);
    }

    win_mutex(win_mutex const&)            = delete;
    win_mutex& operator=(win_mutex const&) = delete;

    void set_enabled(bool v) noexcept { enabled_ = v; }
    bool enabled() const noexcept { return enabled_; }

    void lock() noexcept
    {
        if (enabled_)
            ::EnterCriticalSection(&cs_);
    }

    void unlock() noexcept
    {
        if (enabled_)
            ::LeaveCriticalSection(&cs_);
    }

    bool try_lock() noexcept
    {
        return !enabled_ || ::TryEnterCriticalSection(&cs_) != 0;
    }

private:
    ::CRITICAL_SECTION cs_;
    bool enabled_ = true;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_MUTEX_HPP
