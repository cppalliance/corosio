//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_EPOLL_OP_HPP
#define BOOST_COROSIO_DETAIL_EPOLL_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>
#include <system_error>

#include "src/detail/make_err.hpp"
#include "src/detail/resume_coro.hpp"
#include "src/detail/scheduler_op.hpp"
#include "src/detail/endpoint_convert.hpp"

#include <unistd.h>
#include <errno.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

/*
    epoll Operation State
    =====================

    Each async I/O operation has a corresponding epoll_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Persistent Registration
    -----------------------
    File descriptors are registered with epoll once (via descriptor_data) and
    stay registered until closed. The descriptor_data tracks which operations
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
class epoll_socket_impl;
class epoll_acceptor_impl;
struct epoll_op;

/** Per-descriptor state for persistent epoll registration.

    Tracks pending operations for a file descriptor. The fd is registered
    once with epoll and stays registered until closed. Events are dispatched
    to the appropriate pending operation (EPOLLIN -> read_op, etc.).

    With edge-triggered epoll (EPOLLET), atomic operations are required to
    synchronize between operation registration and reactor event delivery.
    The read_ready/write_ready flags cache edge events that arrived before
    an operation was registered.
*/
struct descriptor_data
{
    /// Currently registered events (EPOLLIN, EPOLLOUT, etc.)
    std::uint32_t registered_events = 0;

    /// Pending read operation (nullptr if none)
    std::atomic<epoll_op*> read_op{nullptr};

    /// Pending write operation (nullptr if none)
    std::atomic<epoll_op*> write_op{nullptr};

    /// Pending connect operation (nullptr if none)
    std::atomic<epoll_op*> connect_op{nullptr};

    /// Cached read readiness (edge event arrived before op registered)
    std::atomic<bool> read_ready{false};

    /// Cached write readiness (edge event arrived before op registered)
    std::atomic<bool> write_ready{false};

    /// The file descriptor
    int fd = -1;

    /// Whether this descriptor is managed by persistent registration
    bool is_registered = false;
};

struct epoll_op : scheduler_op
{
    struct canceller
    {
        epoll_op* op;
        void operator()() const noexcept;
    };

    capy::coro h;
    capy::executor_ref ex;
    std::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    int errn = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    // Prevents use-after-free when socket is closed with pending ops.
    // See "Impl Lifetime Management" in file header.
    std::shared_ptr<void> impl_ptr;

    // For stop_token cancellation - pointer to owning socket/acceptor impl.
    // When stop is requested, we call back to the impl to perform actual I/O cancellation.
    epoll_socket_impl* socket_impl_ = nullptr;
    epoll_acceptor_impl* acceptor_impl_ = nullptr;

    epoll_op()
    {
        data_ = this;
    }

    void reset() noexcept
    {
        fd = -1;
        errn = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        impl_ptr.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = nullptr;
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = capy::error::canceled;
            else if (errn != 0)
                *ec_out = make_err(errn);
            else if (is_read_operation() && bytes_transferred == 0)
                *ec_out = capy::error::eof;
            else
                *ec_out = {};
        }

        if (bytes_out)
            *bytes_out = bytes_transferred;

        // Move to stack before resuming coroutine. The coroutine might close
        // the socket, releasing the last wrapper ref. If impl_ptr were the
        // last ref and we destroyed it while still in operator(), we'd have
        // use-after-free. Moving to local ensures destruction happens at
        // function exit, after all member accesses are complete.
        capy::executor_ref saved_ex( std::move( ex ) );
        capy::coro saved_h( std::move( h ) );
        auto prevent_premature_destruction = std::move(impl_ptr);
        resume_coro(saved_ex, saved_h);
    }

    virtual bool is_read_operation() const noexcept { return false; }
    virtual void cancel() noexcept = 0;

    void destroy() override
    {
        stop_cb.reset();
        impl_ptr.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void start(std::stop_token token, epoll_socket_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = impl;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void start(std::stop_token token, epoll_acceptor_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = impl;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void complete(int err, std::size_t bytes) noexcept
    {
        errn = err;
        bytes_transferred = bytes;
    }

    virtual void perform_io() noexcept {}
};

//------------------------------------------------------------------------------

struct epoll_connect_op : epoll_op
{
    endpoint target_endpoint;

    void reset() noexcept
    {
        epoll_op::reset();
        target_endpoint = endpoint{};
    }

    void perform_io() noexcept override
    {
        // connect() completion status is retrieved via SO_ERROR, not return value
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        complete(err, 0);
    }

    // Defined in sockets.cpp where epoll_socket_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};

//------------------------------------------------------------------------------

struct epoll_read_op : epoll_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;
    bool empty_buffer_read = false;

    bool is_read_operation() const noexcept override
    {
        return !empty_buffer_read;
    }

    void reset() noexcept
    {
        epoll_op::reset();
        iovec_count = 0;
        empty_buffer_read = false;
    }

    void perform_io() noexcept override
    {
        ssize_t n = ::readv(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};

//------------------------------------------------------------------------------

struct epoll_write_op : epoll_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;

    void reset() noexcept
    {
        epoll_op::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        msghdr msg{};
        msg.msg_iov = iovecs;
        msg.msg_iovlen = static_cast<std::size_t>(iovec_count);

        ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};

//------------------------------------------------------------------------------

struct epoll_accept_op : epoll_op
{
    int accepted_fd = -1;
    io_object::io_object_impl* peer_impl = nullptr;
    io_object::io_object_impl** impl_out = nullptr;

    void reset() noexcept
    {
        epoll_op::reset();
        accepted_fd = -1;
        peer_impl = nullptr;
        impl_out = nullptr;
    }

    void perform_io() noexcept override
    {
        sockaddr_in addr{};
        socklen_t addrlen = sizeof(addr);
        int new_fd = ::accept4(fd, reinterpret_cast<sockaddr*>(&addr),
                               &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (new_fd >= 0)
        {
            accepted_fd = new_fd;
            complete(0, 0);
        }
        else
        {
            complete(errno, 0);
        }
    }

    // Defined in acceptors.cpp where epoll_acceptor_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_DETAIL_EPOLL_OP_HPP
