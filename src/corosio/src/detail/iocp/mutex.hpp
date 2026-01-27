//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_MUTEX_HPP
#define BOOST_COROSIO_DETAIL_IOCP_MUTEX_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include <boost/corosio/detail/config.hpp>

#include "src/detail/iocp/windows.hpp"

namespace boost::corosio::detail {

/** Recursive mutex using Windows CRITICAL_SECTION.

    This mutex can be locked multiple times by the same thread.
    Each call to `lock()` or successful `try_lock()` must be
    balanced by a corresponding call to `unlock()`.

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

    win_mutex(win_mutex const&) = delete;
    win_mutex& operator=(win_mutex const&) = delete;

    void
    lock() noexcept
    {
        ::EnterCriticalSection(&cs_);
    }

    void
    unlock() noexcept
    {
        ::LeaveCriticalSection(&cs_);
    }

    bool
    try_lock() noexcept
    {
        return ::TryEnterCriticalSection(&cs_) != 0;
    }

private:
    ::CRITICAL_SECTION cs_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_BACKEND_IOCP

#endif // BOOST_COROSIO_DETAIL_IOCP_MUTEX_HPP
