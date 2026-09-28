//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/native/detail/make_err.hpp>

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

namespace boost::corosio::detail {

/** Validate a caller-supplied socket fd for adoption.

    Non-mutating: interrogates the fd without changing any of its
    flags, so a rejected fd goes back to the caller untouched.

    @param fd The descriptor to validate.
    @param expected_type `SOCK_STREAM` or `SOCK_DGRAM`.
    @param is_ip Accept `AF_INET`/`AF_INET6` when true, `AF_UNIX`
        when false.
    @return Empty on success; `EBADF`, `EAFNOSUPPORT`, `EPROTOTYPE`,
        or the `errno` reported by the interrogating call.
*/
inline std::error_code
validate_socket_fd(int fd, int expected_type, bool is_ip) noexcept
{
    if (fd < 0)
        return make_err(EBADF);

    sockaddr_storage st{};
    socklen_t st_len = sizeof(st);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&st), &st_len) != 0)
        return make_err(errno);
    if (is_ip)
    {
        if (st.ss_family != AF_INET && st.ss_family != AF_INET6)
            return make_err(EAFNOSUPPORT);
    }
    else if (st.ss_family != AF_UNIX)
    {
        return make_err(EAFNOSUPPORT);
    }

    int sock_type     = 0;
    socklen_t opt_len = sizeof(sock_type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &sock_type, &opt_len) != 0)
        return make_err(errno);
    if (sock_type != expected_type)
        return make_err(EPROTOTYPE);

    return {};
}

/** Validate a caller-supplied fd for adoption by @ref posix_descriptor.

    Non-mutating: interrogates the fd without changing any of its
    flags, so a rejected fd goes back to the caller untouched. In
    particular `O_NONBLOCK` is not applied here — see
    @ref ensure_nonblocking.

    The file-type test is a reject-list, not an accept-list. The
    flagship descriptor kinds -- eventfd, timerfd, inotify, pidfd --
    are anonymous inodes whose `st_mode` type bits are all zero, so
    an accept-list would silently reject exactly the fds this type
    exists to carry.

    @param fd The descriptor to validate.
    @return Empty on success; `EBADF` for a closed or negative fd,
        `operation_not_supported` for a regular file, directory or
        block device, or the `errno` reported by `fstat`.
*/
inline std::error_code
validate_descriptor_fd(int fd) noexcept
{
    if (fd < 0)
        return make_err(EBADF);

    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return make_err(errno);

    // Regular files, block devices and directories are the province of
    // stream_file / random_access_file, whose assign() already adopts
    // them; a reactor cannot report readiness for them anyway.
    switch (st.st_mode & S_IFMT)
    {
    case S_IFREG:
    case S_IFBLK:
    case S_IFDIR:
        return std::make_error_code(std::errc::operation_not_supported);
    default:
        return {};
    }
}

/** Validate a caller-supplied fd for adoption by a file object.

    Non-mutating. Accepts the kinds a file object can position and
    read: regular files, block devices, and character devices such
    as /dev/null and /dev/zero.

    This is an accept-list, the inverse of @ref validate_descriptor_fd's
    reject-list: a file object needs a positionable fd, and the
    anonymous inodes that motivate the descriptor reject-list are
    exactly what a file object cannot use.

    `S_IFCHR` is deliberately broad: it admits `/dev/null` and
    `/dev/zero`, but also non-seekable character devices such as a
    tty. Those pass here and then fail loudly at first I/O on the
    POSIX backends, where `preadv`/`pwritev` report `ESPIPE`.

    @param fd The descriptor to validate.
    @return Empty on success; `EBADF` for a closed or negative fd,
        `operation_not_supported` for a directory or a descriptor
        with no file position, or the `errno` from `fstat`.
*/
inline std::error_code
validate_file_fd(int fd) noexcept
{
    if (fd < 0)
        return make_err(EBADF);

    struct stat st{};
    if (::fstat(fd, &st) != 0)
        return make_err(errno);

    switch (st.st_mode & S_IFMT)
    {
    case S_IFREG:
    case S_IFBLK:
    case S_IFCHR:
        return {};
    default:
        return std::make_error_code(std::errc::operation_not_supported);
    }
}

/** Put a descriptor into non-blocking mode, idempotently.

    Called lazily on the first `read_some` / `write_some`, never from
    `assign()`. The change is permanent: `O_NONBLOCK` lives on the
    shared open file description, so restoring it later would race
    every other holder of that description. A `wait()`-only user
    never reaches this function and their fd is never modified.

    @param fd The descriptor to modify.
    @return Empty on success, otherwise the `errno` from `fcntl`.
*/
inline std::error_code
ensure_nonblocking(int fd) noexcept
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return make_err(errno);
    if (flags & O_NONBLOCK)
        return {};
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return make_err(errno);
    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_VALIDATE_FD_HPP
