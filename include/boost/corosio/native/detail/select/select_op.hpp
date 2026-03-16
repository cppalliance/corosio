//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    File descriptors are registered with the select scheduler once (via
    select_descriptor_state) and stay registered until closed.

    select() is level-triggered but the descriptor_state pattern
    (designed for edge-triggered) works correctly: is_enqueued_ CAS
    prevents double-enqueue, add_ready_events is idempotent, and
    EAGAIN ops stay parked until the next select() re-reports readiness.

    cancel() captures shared_from_this() into op.impl_ptr to prevent
    use-after-free when the socket is closed with pending ops.

    Writes use sendmsg(MSG_NOSIGNAL) on Linux. On macOS/BSD where
    MSG_NOSIGNAL may be absent, SO_NOSIGPIPE is set at socket creation
    and accepted-socket setup instead.
*/

namespace boost::corosio::detail {

// Forward declarations
class select_tcp_socket;
class select_tcp_acceptor;
struct select_op;

// Forward declaration
class select_scheduler;

/// Per-descriptor state for persistent select registration.
struct select_descriptor_state final : reactor_descriptor_state
{};

/// select base operation — thin wrapper over reactor_op.
struct select_op : reactor_op<select_tcp_socket, select_tcp_acceptor>
{
    void operator()() override;
};

/// select connect operation.
struct select_connect_op final : reactor_connect_op<select_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// select scatter-read operation.
struct select_read_op final : reactor_read_op<select_op>
{
    void cancel() noexcept override;
};

/** Provides sendmsg() with EINTR retry for select writes.

    Uses MSG_NOSIGNAL where available (Linux). On platforms without
    it (macOS/BSD), SO_NOSIGPIPE is set at socket creation time
    and flags=0 is used here.
*/
struct select_write_policy
{
    static ssize_t write(int fd, iovec* iovecs, int count) noexcept
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(count);

#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif

        ssize_t n;
        do
        {
            n = ::sendmsg(fd, &msg, send_flags);
        }
        while (n < 0 && errno == EINTR);
        return n;
    }
};

/// select gather-write operation.
struct select_write_op final : reactor_write_op<select_op, select_write_policy>
{
    void cancel() noexcept override;
};

/** Provides accept() + fcntl(O_NONBLOCK|FD_CLOEXEC) with FD_SETSIZE check.

    Uses accept() instead of accept4() for broader POSIX compatibility.
*/
struct select_accept_policy
{
    static int do_accept(int fd, sockaddr_storage& peer) noexcept
    {
        socklen_t addrlen = sizeof(peer);
        int new_fd;
        do
        {
            new_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &addrlen);
        }
        while (new_fd < 0 && errno == EINTR);

        if (new_fd < 0)
            return new_fd;

        if (new_fd >= FD_SETSIZE)
        {
            ::close(new_fd);
            errno = EINVAL;
            return -1;
        }

        int flags = ::fcntl(new_fd, F_GETFL, 0);
        if (flags == -1)
        {
            int err = errno;
            ::close(new_fd);
            errno = err;
            return -1;
        }

        if (::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            int err = errno;
            ::close(new_fd);
            errno = err;
            return -1;
        }

        if (::fcntl(new_fd, F_SETFD, FD_CLOEXEC) == -1)
        {
            int err = errno;
            ::close(new_fd);
            errno = err;
            return -1;
        }

#ifdef SO_NOSIGPIPE
        int one = 1;
        if (::setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) ==
            -1)
        {
            int err = errno;
            ::close(new_fd);
            errno = err;
            return -1;
        }
#endif

        return new_fd;
    }
};

/// select accept operation.
struct select_accept_op final
    : reactor_accept_op<select_op, select_accept_policy>
{
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_OP_HPP
