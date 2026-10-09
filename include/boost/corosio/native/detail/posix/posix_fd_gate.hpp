//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_FD_GATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_FD_GATE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <mutex>
#include <vector>

#include <unistd.h>

namespace boost::corosio::detail {

/** Keep a descriptor number allocated while pool workers use it.

    A worker copies the number when its operation is queued and calls
    the kernel later, on another thread. Once the object closes the
    descriptor, the number can be handed to any other open in the
    process, and a worker still on its way to the kernel would then
    transfer through an unrelated file. Workers enter the gate before
    their system call and leave it after; closing while one is inside
    defers the `::close` to the last one out, so the number stays
    allocated to the file the worker meant.

    @par Thread Safety
    Distinct objects: Safe.
    Shared objects: Safe.
*/
class posix_fd_gate
{
    std::mutex mutex_;
    int fd_    = -1;
    int users_ = 0;
    std::vector<int> deferred_;

public:
    /// Destroy the gate, closing any descriptor still deferred.
    ~posix_fd_gate()
    {
        for (int fd : deferred_)
            ::close(fd);
    }

    posix_fd_gate() = default;
    posix_fd_gate(posix_fd_gate const&)            = delete;
    posix_fd_gate& operator=(posix_fd_gate const&) = delete;

    /// Admit workers for @p fd, the descriptor the object now holds.
    void open(int fd) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd_ = fd;
    }

    /** Enter the gate for a system call on @p fd.

        @return False if the object no longer holds @p fd; the caller
            must not use the number.
    */
    bool enter(int fd) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd < 0 || fd != fd_)
            return false;
        ++users_;
        return true;
    }

    /// Leave the gate after a successful @ref enter.
    void leave() noexcept
    {
        std::vector<int> close_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (--users_ == 0)
                close_now.swap(deferred_);
        }
        for (int fd : close_now)
            ::close(fd);
    }

    /// Close the held descriptor once no worker is using it.
    void close() noexcept
    {
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fd  = fd_;
            fd_ = -1;
            if (fd >= 0 && users_ > 0)
            {
                deferred_.push_back(fd);
                fd = -1;
            }
        }
        if (fd >= 0)
            ::close(fd);
    }

    /** Stop admitting workers for the held descriptor, keeping it open.

        @return False, leaving the descriptor admitted, if a worker is
            using it; the caller cannot take ownership of a number a
            worker may still pass to the kernel.
    */
    bool release() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_ > 0)
            return false;
        fd_ = -1;
        return true;
    }

    /// Return true if no descriptor is held and no worker is inside.
    bool idle() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return fd_ == -1 && users_ == 0;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_FD_GATE_HPP
