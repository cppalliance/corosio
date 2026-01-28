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


#if defined(__linux__)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/make_err.hpp"
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

    Completion vs Cancellation Race
    -------------------------------
    The `registered` atomic uses a tri-state (unregistered, registering,
    registered) to handle two races: (1) between register_fd() and the
    reactor seeing an event, and (2) between reactor completion and cancel().

    The registering state closes the window where an event could arrive
    after register_fd() but before the boolean was set. The reactor and
    cancel() both treat registering the same as registered when claiming.

    Whoever atomically exchanges to unregistered "claims" the operation
    and is responsible for completing it. The loser sees unregistered and
    does nothing. The initiating thread uses compare_exchange to transition
    from registering to registered; if this fails, the reactor or cancel
    already claimed the op. This avoids double-completion bugs without
    requiring a mutex in the hot path.

    Impl Lifetime Management
    ------------------------
    When cancel() posts an op to the scheduler's ready queue, the socket impl
    might be destroyed before the scheduler processes the op. The `impl_ptr`
    member holds a shared_ptr to the impl, keeping it alive until the op
    completes. This is set by cancel() in sockets.hpp and cleared in operator()
    after the coroutine is resumed. Without this, closing a socket with pending
    operations causes use-after-free.

    EOF Detection
    -------------
    For reads, 0 bytes with no error means EOF. But an empty user buffer also
    returns 0 bytes. The `empty_buffer_read` flag distinguishes these cases
    so we don't spuriously report EOF when the user just passed an empty buffer.

    SIGPIPE Prevention
    ------------------
    Writes use sendmsg() with MSG_NOSIGNAL instead of writev() to prevent
    SIGPIPE when the peer has closed. This is the same approach Boost.Asio
    uses on Linux.
*/

namespace boost::corosio::detail {

// Forward declarations for cancellation support
class epoll_socket_impl;
class epoll_acceptor_impl;

/** Registration state for async operations.

    Tri-state enum to handle the race between register_fd() and
    run_reactor() seeing an event. Setting REGISTERING before
    calling register_fd() ensures events delivered during the
    registration window are not dropped.
*/
enum class registration_state : std::uint8_t
{
    unregistered,  ///< Not registered with reactor
    registering,   ///< register_fd() called, not yet confirmed
    registered     ///< Fully registered, ready for events
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
    system::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    int errn = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::atomic<registration_state> registered{registration_state::unregistered};
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
        registered.store(registration_state::unregistered, std::memory_order_relaxed);
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
        }

        if (bytes_out)
            *bytes_out = bytes_transferred;

        // Move to stack before destroying the frame
        capy::executor_ref saved_ex( std::move( ex ) );
        capy::coro saved_h( std::move( h ) );
        impl_ptr.reset();
        saved_ex.dispatch( saved_h ).resume();
    }

    virtual bool is_read_operation() const noexcept { return false; }

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

    // Defined in sockets.cpp where epoll_socket_impl is complete
    void operator()() override;
};

} // namespace boost::corosio::detail

#endif // __linux__

#endif // BOOST_COROSIO_DETAIL_EPOLL_OP_HPP
