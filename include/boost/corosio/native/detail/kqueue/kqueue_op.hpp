//
// Copyright (c) 2026 Michael Vandeberg
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <fcntl.h>
#include <unistd.h>

/*
    kqueue Operation State
    ======================

    Each async I/O operation has a corresponding kqueue_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Persistent Registration
    -----------------------
    File descriptors are registered with kqueue once (via descriptor_state) and
    stay registered until closed. Uses EV_CLEAR for edge-triggered semantics
    (equivalent to epoll's EPOLLET). The descriptor_state tracks which operations
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
    SO_NOSIGPIPE is set on each socket at creation time (see sockets.cpp).
    Writes use writev() which is safe because the socket-level option suppresses
    SIGPIPE delivery.
*/

namespace boost::corosio::detail {

// Aliases for shared reactor event constants.
// Kept for backward compatibility in kqueue-specific code.
static constexpr std::uint32_t kqueue_event_read  = reactor_event_read;
static constexpr std::uint32_t kqueue_event_write = reactor_event_write;
static constexpr std::uint32_t kqueue_event_error = reactor_event_error;

// Forward declarations
class kqueue_tcp_socket;
class kqueue_tcp_acceptor;
struct kqueue_op;

class kqueue_scheduler;

/// Per-descriptor state for persistent kqueue registration.
struct descriptor_state final : reactor_descriptor_state
{};

/// kqueue base operation — thin wrapper over reactor_op.
struct kqueue_op : reactor_op<kqueue_tcp_socket, kqueue_tcp_acceptor>
{
    void operator()() override;
};

/// kqueue connect operation.
struct kqueue_connect_op final : reactor_connect_op<kqueue_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// kqueue scatter-read operation.
struct kqueue_read_op final : reactor_read_op<kqueue_op>
{
    void cancel() noexcept override;
};

/** Provides writev() for kqueue writes.

    SO_NOSIGPIPE is set on the socket at creation time (macOS lacks
    MSG_NOSIGNAL), so writev() is safe from SIGPIPE.
*/
struct kqueue_write_policy
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
};

/// kqueue gather-write operation.
struct kqueue_write_op final : reactor_write_op<kqueue_op, kqueue_write_policy>
{
    void cancel() noexcept override;
};

/** Provides accept() + fcntl() + SO_NOSIGPIPE for kqueue accepts.

    Unlike Linux's accept4(), BSD accept() does not support atomic
    flag setting. Non-blocking, close-on-exec, and SIGPIPE suppression
    are applied via separate syscalls after accept().
*/
struct kqueue_accept_policy
{
    static int do_accept(int fd, sockaddr_storage& peer) noexcept
    {
        int new_fd;
        do
        {
            socklen_t addrlen = sizeof(peer);
            new_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &addrlen);
        }
        while (new_fd < 0 && errno == EINTR);

        if (new_fd < 0)
            return new_fd;

        int flags = ::fcntl(new_fd, F_GETFL, 0);
        if (flags == -1 || ::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
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

        // macOS lacks MSG_NOSIGNAL
        int one = 1;
        if (::setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) ==
            -1)
        {
            int err = errno;
            ::close(new_fd);
            errno = err;
            return -1;
        }

        return new_fd;
    }
};

/// kqueue accept operation.
struct kqueue_accept_op final
    : reactor_accept_op<kqueue_op, kqueue_accept_policy>
{
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_OP_HPP
