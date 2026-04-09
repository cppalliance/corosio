//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/local_socket_pair.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio {

namespace {

#ifndef SOCK_NONBLOCK
// Only needed on platforms lacking SOCK_NONBLOCK/SOCK_CLOEXEC (e.g. older BSDs).
// On Linux/glibc SOCK_NONBLOCK is always defined, so omitting this definition
// avoids a -Wunused-function diagnostic.
//
// Returns 0 on success or errno on failure. Caller owns fd on error.
int
make_nonblock_cloexec(int fd)
{
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl == -1)
        return errno;
    if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == -1)
        return errno;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        return errno;
    return 0;
}
#endif

void
create_pair(int type, int fds[2])
{
    int flags = type;
#ifdef SOCK_NONBLOCK
    flags |= SOCK_NONBLOCK | SOCK_CLOEXEC;
#endif
    if (::socketpair(AF_UNIX, flags, 0, fds) != 0)
        throw std::system_error(
            std::error_code(errno, std::system_category()),
            "socketpair");
#ifndef SOCK_NONBLOCK
    int err = make_nonblock_cloexec(fds[0]);
    if (err == 0)
        err = make_nonblock_cloexec(fds[1]);
    if (err != 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::system_error(
            std::error_code(err, std::system_category()),
            "socketpair (fcntl setup)");
    }
#endif
}

} // namespace

std::pair<local_stream_socket, local_stream_socket>
make_local_stream_pair(io_context& ctx)
{
    int fds[2];
    create_pair(SOCK_STREAM, fds);

    try
    {
        local_stream_socket s1(ctx);
        local_stream_socket s2(ctx);

        s1.assign(fds[0]);
        fds[0] = -1;
        s2.assign(fds[1]);
        fds[1] = -1;

        return {std::move(s1), std::move(s2)};
    }
    catch (...)
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
        throw;
    }
}

std::pair<local_datagram_socket, local_datagram_socket>
make_local_datagram_pair(io_context& ctx)
{
    int fds[2];
    create_pair(SOCK_DGRAM, fds);

    try
    {
        local_datagram_socket s1(ctx);
        local_datagram_socket s2(ctx);

        s1.assign(fds[0]);
        fds[0] = -1;
        s2.assign(fds[1]);
        fds[1] = -1;

        return {std::move(s1), std::move(s2)};
    }
    catch (...)
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
        throw;
    }
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
