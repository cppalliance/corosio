//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_LARGE_FILE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_LARGE_FILE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* 64-bit file offsets in every build.

   32-bit glibc has a 32-bit off_t unless the including translation
   unit defines _FILE_OFFSET_BITS=64, which the library cannot require
   of its users. The plain calls then fail past 2 GiB: EOVERFLOW for an
   offset that does not fit, and EFBIG for a file opened without
   O_LARGEFILE. There the explicit *64 calls and O_LARGEFILE are used.
   Everywhere else off_t is already 64-bit, and the plain calls are
   kept, so the fault harness's shadows of them still apply.
*/

namespace boost::corosio::detail {

#if defined(__GLIBC__) && !defined(__OFF_T_MATCHES_OFF64_T)

using file_off_t  = off64_t;
using file_stat_t = struct stat64;

/// Add to the flags of every open() of a file the library owns.
inline constexpr int large_file_open_flag = O_LARGEFILE;

inline ssize_t
file_preadv(int fd, iovec const* iov, int n, file_off_t off) noexcept
{
    return ::preadv64(fd, iov, n, off);
}

inline ssize_t
file_pwritev(int fd, iovec const* iov, int n, file_off_t off) noexcept
{
    return ::pwritev64(fd, iov, n, off);
}

inline int
file_fstat(int fd, file_stat_t* st) noexcept
{
    return ::fstat64(fd, st);
}

inline int
file_ftruncate(int fd, file_off_t len) noexcept
{
    return ::ftruncate64(fd, len);
}

inline file_off_t
file_lseek(int fd, file_off_t off, int whence) noexcept
{
    return ::lseek64(fd, off, whence);
}

#else

using file_off_t  = off_t;
using file_stat_t = struct stat;

/// Add to the flags of every open() of a file the library owns.
inline constexpr int large_file_open_flag = 0;

inline ssize_t
file_preadv(int fd, iovec const* iov, int n, file_off_t off) noexcept
{
    return ::preadv(fd, iov, n, off);
}

inline ssize_t
file_pwritev(int fd, iovec const* iov, int n, file_off_t off) noexcept
{
    return ::pwritev(fd, iov, n, off);
}

inline int
file_fstat(int fd, file_stat_t* st) noexcept
{
    return ::fstat(fd, st);
}

inline int
file_ftruncate(int fd, file_off_t len) noexcept
{
    return ::ftruncate(fd, len);
}

inline file_off_t
file_lseek(int fd, file_off_t off, int whence) noexcept
{
    return ::lseek(fd, off, whence);
}

#endif

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_LARGE_FILE_HPP
