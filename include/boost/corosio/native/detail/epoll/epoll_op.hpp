//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

/*
    epoll Operation State
    =====================

    Each async I/O operation has a corresponding epoll_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Persistent Registration
    -----------------------
    File descriptors are registered with epoll once (via descriptor_state) and
    stay registered until closed. The descriptor_state tracks which operations
    are pending (read_op, write_op, connect_op). When an event arrives, the
    reactor dispatches to the appropriate pending operation.

    Impl Lifetime Management
    ------------------------
    When cancel() posts an op to the scheduler's ready queue, the socket impl
    might be destroyed before the scheduler processes the op. The `impl_ptr`
    member holds a shared_ptr to the impl, keeping it alive until the op
    completes. This is set by cancel() and cleared in operator() after the
    coroutine is resumed.

    EOF Detection
    -------------
    For reads, 0 bytes with no error means EOF. But an empty user buffer also
    returns 0 bytes. The `empty_buffer_read` flag distinguishes these cases.

    SIGPIPE Prevention
    ------------------
    Writes use sendmsg() with MSG_NOSIGNAL instead of writev() to prevent
    SIGPIPE when the peer has closed.
*/

namespace boost::corosio::detail {

// Forward declarations
class epoll_tcp_socket;
class epoll_tcp_acceptor;
struct epoll_op;

// Forward declaration
class epoll_scheduler;

/// Per-descriptor state for persistent epoll registration.
struct descriptor_state final : reactor_descriptor_state
{};

/// epoll base operation — thin wrapper over reactor_op.
struct epoll_op : reactor_op<epoll_tcp_socket, epoll_tcp_acceptor>
{
    void operator()() override;
};

/// epoll connect operation.
struct epoll_connect_op final : reactor_connect_op<epoll_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// epoll scatter-read operation.
struct epoll_read_op final : reactor_read_op<epoll_op>
{
    void cancel() noexcept override;
};

/** Provides sendmsg(MSG_NOSIGNAL) with EINTR retry for epoll writes. */
struct epoll_write_policy
{
    static ssize_t write(int fd, iovec* iovecs, int count) noexcept
    {
        msghdr msg{};
        msg.msg_iov    = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(count);

        ssize_t n;
        do
        {
            n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        }
        while (n < 0 && errno == EINTR);
        return n;
    }
};

/// epoll gather-write operation.
struct epoll_write_op final : reactor_write_op<epoll_op, epoll_write_policy>
{
    void cancel() noexcept override;
};

/** Provides accept4(SOCK_NONBLOCK|SOCK_CLOEXEC) with EINTR retry. */
struct epoll_accept_policy
{
    static int do_accept(
        int fd, sockaddr_storage& peer, socklen_t& addrlen_out) noexcept
    {
        addrlen_out = sizeof(peer);
        int new_fd;
        do
        {
            new_fd = ::accept4(
                fd, reinterpret_cast<sockaddr*>(&peer), &addrlen_out,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
        }
        while (new_fd < 0 && errno == EINTR);
        return new_fd;
    }
};

/// epoll accept operation.
struct epoll_accept_op final : reactor_accept_op<epoll_op, epoll_accept_policy>
{
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_OP_HPP
