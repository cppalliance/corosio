//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_OPS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <cstddef>

#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace boost::corosio::detail {

/* Write policy shared by every reactor backend's descriptor ops.

   Descriptors are not sockets: sendmsg() fails with ENOTSOCK on a pipe
   or character device, so the write path is writev()/write() and
   SIGPIPE suppression is structurally unavailable -- MSG_NOSIGNAL is a
   send() flag and SO_NOSIGPIPE a socket option. A write to a pipe whose
   read end has closed raises SIGPIPE, exactly as a plain write(2)
   would; callers install SIG_IGN.
*/
struct descriptor_write_policy
{
    static ssize_t write(int fd, iovec* iovecs, int count) noexcept
    {
        ssize_t n;
        do
        {
            n = ::writev(fd, iovecs, count);
        }
        while (n < 0 && errno == EINTR);
        return n;
    }

    // Single-buffer fast path: skips the kernel's iov_iter setup.
    static ssize_t
    write_one(int fd, void const* data, std::size_t size) noexcept
    {
        ssize_t n;
        do
        {
            n = ::write(fd, data, size);
        }
        while (n < 0 && errno == EINTR);
        return n;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_OPS_HPP
